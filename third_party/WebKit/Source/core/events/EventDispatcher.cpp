/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011 Apple Inc. All rights reserved.
 * Copyright (C) 2008 Nokia Corporation and/or its subsidiary(-ies)
 * Copyright (C) 2009 Torch Mobile Inc. All rights reserved. (http://www.torchmobile.com/)
 * Copyright (C) 2011 Google Inc. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "core/events/EventDispatcher.h"

#include "core/dom/ContainerNode.h"
#include "core/events/EventDispatchMediator.h"
#include "core/events/MouseEvent.h"
#include "core/events/ScopedEventQueue.h"
#include "core/events/WindowEventContext.h"
#include "core/frame/FrameView.h"
#include "core/inspector/InspectorInstrumentation.h"
#include "core/inspector/InspectorTraceEvents.h"
#include "platform/EventDispatchForbiddenScope.h"
#include "platform/TraceEvent.h"
#include "wtf/RefPtr.h"

namespace blink {

bool EventDispatcher::dispatchEvent(Node* node, PassRefPtrWillBeRawPtr<EventDispatchMediator> mediator)
{
    TRACE_EVENT0("blink", "EventDispatcher::dispatchEvent");
    ASSERT(!EventDispatchForbiddenScope::isEventDispatchForbidden());
    if (!mediator->event())
        return true;
    EventDispatcher dispatcher(node, mediator->event());//从TouchEventDispatcherMediator对象中取出封装的Touch Event，并将该Touch Event封装在EventDispatcher
    return mediator->dispatchEvent(&dispatcher);//调用TouchEventDispatcherMediator对象的dispatchEvent
}

EventDispatcher::EventDispatcher(Node* node, PassRefPtrWillBeRawPtr<Event> event)
    : m_node(node)
    , m_event(event)
#if ENABLE(ASSERT)
    , m_eventDispatched(false)
#endif
{
    ASSERT(node);
    ASSERT(m_event.get());
    ASSERT(!m_event->type().isNull()); // JavaScript code can create an event with an empty name, but not null.
    m_view = node->document().view();
    m_event->ensureEventPath().resetWith(m_node.get());
}

void EventDispatcher::dispatchScopedEvent(Node* node, PassRefPtrWillBeRawPtr<EventDispatchMediator> mediator)
{
    // We need to set the target here because it can go away by the time we actually fire the event.
    mediator->event()->setTarget(EventPath::eventTargetRespectingTargetRules(node));
    ScopedEventQueue::instance()->enqueueEventDispatchMediator(mediator);
}

void EventDispatcher::dispatchSimulatedClick(Node* node, Event* underlyingEvent, SimulatedClickMouseEventOptions mouseEventOptions)
{
    // This persistent vector doesn't cause leaks, because added Nodes are removed
    // before dispatchSimulatedClick() returns. This vector is here just to prevent
    // the code from running into an infinite recursion of dispatchSimulatedClick().
    DEFINE_STATIC_LOCAL(OwnPtrWillBePersistent<WillBeHeapHashSet<RawPtrWillBeMember<Node> > >, nodesDispatchingSimulatedClicks, (adoptPtrWillBeNoop(new WillBeHeapHashSet<RawPtrWillBeMember<Node> >())));

    if (isDisabledFormControl(node))
        return;

    if (nodesDispatchingSimulatedClicks->contains(node))
        return;

    nodesDispatchingSimulatedClicks->add(node);

    if (mouseEventOptions == SendMouseOverUpDownEvents)
        EventDispatcher(node, SimulatedMouseEvent::create(EventTypeNames::mouseover, node->document().domWindow(), underlyingEvent)).dispatch();

    if (mouseEventOptions != SendNoEvents) {
        EventDispatcher(node, SimulatedMouseEvent::create(EventTypeNames::mousedown, node->document().domWindow(), underlyingEvent)).dispatch();
        node->setActive(true);
        EventDispatcher(node, SimulatedMouseEvent::create(EventTypeNames::mouseup, node->document().domWindow(), underlyingEvent)).dispatch();
    }
    // Some elements (e.g. the color picker) may set active state to true before
    // calling this method and expect the state to be reset during the call.
    node->setActive(false);

    // always send click
    EventDispatcher(node, SimulatedMouseEvent::create(EventTypeNames::click, node->document().domWindow(), underlyingEvent)).dispatch();

    nodesDispatchingSimulatedClicks->remove(node);
}

bool EventDispatcher::dispatch()
{
    TRACE_EVENT0("blink", "EventDispatcher::dispatch");

#if ENABLE(ASSERT)
    ASSERT(!m_eventDispatched);
    m_eventDispatched = true;
#endif

    m_event->setTarget(EventPath::eventTargetRespectingTargetRules(m_node.get()));
    ASSERT(!EventDispatchForbiddenScope::isEventDispatchForbidden());
    ASSERT(m_event->target());
    WindowEventContext windowEventContext(m_event.get(), m_node.get(), topNodeEventContext());
    TRACE_EVENT1(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "EventDispatch", "data", InspectorEventDispatchEvent::data(*m_event));
    // FIXME(361045): remove InspectorInstrumentation calls once DevTools Timeline migrates to tracing.
    InspectorInstrumentationCookie cookie = InspectorInstrumentation::willDispatchEvent(&m_node->document(), *m_event, windowEventContext.window(), m_node.get(), m_event->eventPath());

    void* preDispatchEventHandlerResult;
    //网页发生的一个Event,分为五个阶段进行分发处理：
    //1. Pre Process
    //2. Capture
    //3. Target
    //4. Bubble
    //5. Post Process
    /* 
    我们通过一个例子说明这五个阶段的处理过程，如下所示：
    <html>  
        <body>  
            <div id="div1">   
                <div id="div2">  
                    <div id="div3">  
                    </div>  
                </div>  
            </div>  
        </body>  
    </html>  
       假设在div3上发生了一个Touch Event。
       在Pre Process阶段，WebKit会将Touch Event分发给div3的Pre Dispatch Event Handler处理，让div3有机会在DOM Event Handler处理Touch Event之前做一些事情，用来实现自己的行为。

       在Capturing阶段，WebKit会将Touch Event依次分发给html -> body -> div1 -> div2的DOM Event Handler处理。

       在Target阶段，WebKit会将Touch Event依次分发给div3的DOM Event Handler处理。。

       在Bubbling阶段， WebKit会将Touch Event依次分发给div2 -> div1 -> body -> html的DOM Event Handler处理。

       在Post Process阶段，WebKit会将Touch Event分发给div3的Post Dispatch Event Handler处理，让div3有机会在DOM Event Handler处理Touch Event之后做一些事情，与它的Pre Dispatch Event Handler相呼应。

       此外，如果在前面4个阶段，Touch Event的preventDefault函数没有被调用，那么WebKit会将它依次分发给div3 -> div2 -> div1 -> body -> html的Default Event Handler处理。在这个过程中，如果某一个Node的Default Event Handler处理了该Touch Event，那么该Touch Event的分发过程就会中止。

       其中，Pre Process和Post Process这两个阶段是一定会执行的。在Capturing、Target和Bubbling这三个阶段，如果某一个Node的DOM Event Handler调用了Touch Event的stopPropagation函数，那么它就会提前中止，后面的阶段也不会被执行。

       Pre Dispatch Event Handler、Post Dispatch Event Handler和Default Event Handler是由WebKit实现的，DOM Event Handler可以通过JavaScript进行注册。在注册的时候，可以指定DOM Event Handler在Capturing阶段还是Bubbling阶段接收Event，但是不能同时在这两个阶段都接收。此外，注册Target Node上的DOM Event Handler没有Capturing阶段还是Bubbling阶段之分，如果Event在Capturing阶段没有被中止，那么它将在Target阶段接收。

       明白了WebKit中的Event处理流程之后，接下来我们主要分析Target Node在Target阶段处理Touch Event的过程，也就是EventDispatcher类的成员函数dispatchEventAtTarget的实现
       */
    if (dispatchEventPreProcess(preDispatchEventHandlerResult) == ContinueDispatching)
        if (dispatchEventAtCapturing(windowEventContext) == ContinueDispatching)
            if (dispatchEventAtTarget() == ContinueDispatching)
                dispatchEventAtBubbling(windowEventContext);
    dispatchEventPostProcess(preDispatchEventHandlerResult);

    // Ensure that after event dispatch, the event's target object is the
    // outermost shadow DOM boundary.
    m_event->setTarget(windowEventContext.target());
    m_event->setCurrentTarget(0);
    InspectorInstrumentation::didDispatchEvent(cookie);
    TRACE_EVENT_INSTANT1(TRACE_DISABLED_BY_DEFAULT("devtools.timeline"), "UpdateCounters", "data", InspectorUpdateCountersEvent::data());

    return !m_event->defaultPrevented();
}

inline EventDispatchContinuation EventDispatcher::dispatchEventPreProcess(void*& preDispatchEventHandlerResult)
{
    // Give the target node a chance to do some work before DOM event handlers get a crack.
    preDispatchEventHandlerResult = m_node->preDispatchEventHandler(m_event.get());
    return (m_event->eventPath().isEmpty() || m_event->propagationStopped()) ? DoneDispatching : ContinueDispatching;
}

inline EventDispatchContinuation EventDispatcher::dispatchEventAtCapturing(WindowEventContext& windowEventContext)
{
    // Trigger capturing event handlers, starting at the top and working our way down.
    m_event->setEventPhase(Event::CAPTURING_PHASE);

    if (windowEventContext.handleLocalEvents(m_event.get()) && m_event->propagationStopped())
        return DoneDispatching;

    for (size_t i = m_event->eventPath().size() - 1; i > 0; --i) {
        const NodeEventContext& eventContext = m_event->eventPath()[i];
        if (eventContext.currentTargetSameAsTarget())
            continue;
        eventContext.handleLocalEvents(m_event.get());
        if (m_event->propagationStopped())
            return DoneDispatching;
    }

    return ContinueDispatching;
}

inline EventDispatchContinuation EventDispatcher::dispatchEventAtTarget()
{
    m_event->setEventPhase(Event::AT_TARGET);
    m_event->eventPath()[0].handleLocalEvents(m_event.get());//eventPath是一个Node Event Context列表，列表中的第一个Node Event Context描述的是当前发生的TOuch Event的target node的上下文信息。
    return m_event->propagationStopped() ? DoneDispatching : ContinueDispatching;
}

inline void EventDispatcher::dispatchEventAtBubbling(WindowEventContext& windowContext)
{
    // Trigger bubbling event handlers, starting at the bottom and working our way up.
    size_t size = m_event->eventPath().size();
    for (size_t i = 1; i < size; ++i) {
        const NodeEventContext& eventContext = m_event->eventPath()[i];
        if (eventContext.currentTargetSameAsTarget())
            m_event->setEventPhase(Event::AT_TARGET);
        else if (m_event->bubbles() && !m_event->cancelBubble())
            m_event->setEventPhase(Event::BUBBLING_PHASE);
        else
            continue;
        eventContext.handleLocalEvents(m_event.get());
        if (m_event->propagationStopped())
            return;
    }
    if (m_event->bubbles() && !m_event->cancelBubble()) {
        m_event->setEventPhase(Event::BUBBLING_PHASE);
        windowContext.handleLocalEvents(m_event.get());
    }
}

inline void EventDispatcher::dispatchEventPostProcess(void* preDispatchEventHandlerResult)
{
    m_event->setTarget(EventPath::eventTargetRespectingTargetRules(m_node.get()));
    m_event->setCurrentTarget(0);
    m_event->setEventPhase(0);

    // Pass the data from the preDispatchEventHandler to the postDispatchEventHandler.
    m_node->postDispatchEventHandler(m_event.get(), preDispatchEventHandlerResult);

    // Call default event handlers. While the DOM does have a concept of preventing
    // default handling, the detail of which handlers are called is an internal
    // implementation detail and not part of the DOM.
    if (!m_event->defaultPrevented() && !m_event->defaultHandled()) {
        // Non-bubbling events call only one default event handler, the one for the target.
        m_node->willCallDefaultEventHandler(*m_event);
        m_node->defaultEventHandler(m_event.get());
        ASSERT(!m_event->defaultPrevented());
        if (m_event->defaultHandled())
            return;
        // For bubbling events, call default event handlers on the same targets in the
        // same order as the bubbling phase.
        if (m_event->bubbles()) {
            size_t size = m_event->eventPath().size();
            for (size_t i = 1; i < size; ++i) {
                m_event->eventPath()[i].node()->willCallDefaultEventHandler(*m_event);
                m_event->eventPath()[i].node()->defaultEventHandler(m_event.get());
                ASSERT(!m_event->defaultPrevented());
                if (m_event->defaultHandled())
                    return;
            }
        }
    }
}

const NodeEventContext* EventDispatcher::topNodeEventContext()
{
    return m_event->eventPath().isEmpty() ? 0 : &m_event->eventPath().last();
}

}
