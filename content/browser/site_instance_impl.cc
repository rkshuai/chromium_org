// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/site_instance_impl.h"

#include "base/command_line.h"
#include "content/browser/browsing_instance.h"
#include "content/browser/child_process_security_policy_impl.h"
#include "content/browser/frame_host/debug_urls.h"
#include "content/browser/renderer_host/render_process_host_impl.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/render_process_host_factory.h"
#include "content/public/browser/web_ui_controller_factory.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/url_constants.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"

namespace content {

const RenderProcessHostFactory*
    SiteInstanceImpl::g_render_process_host_factory_ = NULL;
int32 SiteInstanceImpl::next_site_instance_id_ = 1;

SiteInstanceImpl::SiteInstanceImpl(BrowsingInstance* browsing_instance)
    : id_(next_site_instance_id_++),
      active_view_count_(0),
      browsing_instance_(browsing_instance),
      process_(NULL),
      has_site_(false) {
  DCHECK(browsing_instance);
}

SiteInstanceImpl::~SiteInstanceImpl() {
  GetContentClient()->browser()->SiteInstanceDeleting(this);

  if (process_)
    process_->RemoveObserver(this);

  // Now that no one is referencing us, we can safely remove ourselves from
  // the BrowsingInstance.  Any future visits to a page from this site
  // (within the same BrowsingInstance) can safely create a new SiteInstance.
  if (has_site_)
    browsing_instance_->UnregisterSiteInstance(
        static_cast<SiteInstance*>(this));
}

int32 SiteInstanceImpl::GetId() {
  return id_;
}

bool SiteInstanceImpl::HasProcess() const {
  if (process_ != NULL)
    return true;

  // If we would use process-per-site for this site, also check if there is an
  // existing process that we would use if GetProcess() were called.
  BrowserContext* browser_context =
      browsing_instance_->browser_context();
  if (has_site_ &&
      RenderProcessHost::ShouldUseProcessPerSite(browser_context, site_) &&
      RenderProcessHostImpl::GetProcessHostForSite(browser_context, site_)) {
    return true;
  }

  return false;
}

RenderProcessHost* SiteInstanceImpl::GetProcess() {
  // TODO(erikkay) It would be nice to ensure that the renderer type had been
  // properly set before we get here.  The default tab creation case winds up
  // with no site set at this point, so it will default to TYPE_NORMAL.  This
  // may not be correct, so we'll wind up potentially creating a process that
  // we then throw away, or worse sharing a process with the wrong process type.
  // See crbug.com/43448.

  // Create a new process if ours went away or was reused.
  if (!process_) {//当它的值等于NULL的时候，就表示Chromium还没有为当前正在处理的一个SiteInstanceImpl对象创建过Render进程，这时候就需要创建一个RenderProcessHostImpl对象
    BrowserContext* browser_context = browsing_instance_->browser_context();

    // If we should use process-per-site mode (either in general or for the
    // given site), then look for an existing RenderProcessHost for the site.
    bool use_process_per_site = has_site_ &&
        RenderProcessHost::ShouldUseProcessPerSite(browser_context, site_);
    //如果Chromium启动时指定同一个网站的所有网页都在同一个Render进程中加载，即use_process_per_site=true，这时调用GetProcessHostForSite检查之前是否已经为当前正在处理的SiteInstanceImpl对象描述的网站创建过Render进程。如果已经创建过，那么就可以直接获得一个对应的RenderProcessHostImpl对象
    if (use_process_per_site) {
      process_ = RenderProcessHostImpl::GetProcessHostForSite(browser_context,
                                                              site_);
    }

    // If not (or if none found), see if we should reuse an existing process.
    //如果按照上面方法没找到，这时本来应该创建一个Render进程，也就是RenderProcessHostImpl对象，但是由于当前创建的Render进程已经超出预设的最大数量了，这时就要复用前面已经启动的Render进程，即使这个Render进程加载的是另一个网站的内容。
    if (!process_ && RenderProcessHostImpl::ShouldTryToUseExistingProcessHost(
            browser_context, site_)) {
      process_ = RenderProcessHostImpl::GetExistingProcessHost(browser_context,
                                                               site_);
    }

    // Otherwise (or if that fails), create a new one.
    //如果上面两个条件都不满足，那么真的需要创建一个RenderProcessHostImpl对象了。
    if (!process_) {
      if (g_render_process_host_factory_) {//如果g_render_process_host_factory_指向一个RenderProcessHostFactory对象，那么调用CreateRenderProcessHost创建一个从RenderProcessHost类继承下来的子类对象
        process_ = g_render_process_host_factory_->CreateRenderProcessHost(
            browser_context, this);
      } else {//否则创建一个RenderProcessHostImpl对象
        StoragePartitionImpl* partition =
            static_cast<StoragePartitionImpl*>(
                BrowserContext::GetStoragePartition(browser_context, this));
        process_ = new RenderProcessHostImpl(browser_context,
                                             partition,
                                             site_.SchemeIs(kGuestScheme));
      }
    }
    CHECK(process_);
    process_->AddObserver(this);

    // If we are using process-per-site, we need to register this process
    // for the current site so that we can find it again.  (If no site is set
    // at this time, we will register it in SetSite().)
    if (use_process_per_site) {
      RenderProcessHostImpl::RegisterProcessHostForSite(browser_context,
                                                        process_, site_);
    }

    TRACE_EVENT2("navigation", "SiteInstanceImpl::GetProcess",
                 "site id", id_, "process id", process_->GetID());
    GetContentClient()->browser()->SiteInstanceGotProcess(this);

    if (has_site_)
      LockToOrigin();
  }
  DCHECK(process_);

  return process_;
}

void SiteInstanceImpl::SetSite(const GURL& url) {
  TRACE_EVENT2("navigation", "SiteInstanceImpl::SetSite",
               "site id", id_, "url", url.possibly_invalid_spec());
  // A SiteInstance's site should not change.
  // TODO(creis): When following links or script navigations, we can currently
  // render pages from other sites in this SiteInstance.  This will eventually
  // be fixed, but until then, we should still not set the site of a
  // SiteInstance more than once.
  DCHECK(!has_site_);

  // Remember that this SiteInstance has been used to load a URL, even if the
  // URL is invalid.
  has_site_ = true;
  BrowserContext* browser_context = browsing_instance_->browser_context();
  site_ = GetSiteForURL(browser_context, url);

  // Now that we have a site, register it with the BrowsingInstance.  This
  // ensures that we won't create another SiteInstance for this site within
  // the same BrowsingInstance, because all same-site pages within a
  // BrowsingInstance can script each other.
  browsing_instance_->RegisterSiteInstance(this);

  if (process_) {
    LockToOrigin();

    // Ensure the process is registered for this site if necessary.
    if (RenderProcessHost::ShouldUseProcessPerSite(browser_context, site_)) {
      RenderProcessHostImpl::RegisterProcessHostForSite(
          browser_context, process_, site_);
    }
  }
}

const GURL& SiteInstanceImpl::GetSiteURL() const {
  return site_;
}

bool SiteInstanceImpl::HasSite() const {
  return has_site_;
}

bool SiteInstanceImpl::HasRelatedSiteInstance(const GURL& url) {
  return browsing_instance_->HasSiteInstance(url);
}

SiteInstance* SiteInstanceImpl::GetRelatedSiteInstance(const GURL& url) {
  return browsing_instance_->GetSiteInstanceForURL(url);
}

bool SiteInstanceImpl::IsRelatedSiteInstance(const SiteInstance* instance) {
  return browsing_instance_.get() == static_cast<const SiteInstanceImpl*>(
                                         instance)->browsing_instance_.get();
}

size_t SiteInstanceImpl::GetRelatedActiveContentsCount() {
  return browsing_instance_->active_contents_count();
}

bool SiteInstanceImpl::HasWrongProcessForURL(const GURL& url) {
  // Having no process isn't a problem, since we'll assign it correctly.
  // Note that HasProcess() may return true if process_ is null, in
  // process-per-site cases where there's an existing process available.
  // We want to use such a process in the IsSuitableHost check, so we
  // may end up assigning process_ in the GetProcess() call below.
  if (!HasProcess())
    return false;

  // If the URL to navigate to can be associated with any site instance,
  // we want to keep it in the same process.
  if (IsRendererDebugURL(url))
    return false;

  // If the site URL is an extension (e.g., for hosted apps or WebUI) but the
  // process is not (or vice versa), make sure we notice and fix it.
  GURL site_url = GetSiteForURL(browsing_instance_->browser_context(), url);
  return !RenderProcessHostImpl::IsSuitableHost(
      GetProcess(), browsing_instance_->browser_context(), site_url);
}

void SiteInstanceImpl::IncrementRelatedActiveContentsCount() {
  browsing_instance_->increment_active_contents_count();
}

void SiteInstanceImpl::DecrementRelatedActiveContentsCount() {
  browsing_instance_->decrement_active_contents_count();
}

void SiteInstanceImpl::set_render_process_host_factory(
    const RenderProcessHostFactory* rph_factory) {
  g_render_process_host_factory_ = rph_factory;
}

BrowserContext* SiteInstanceImpl::GetBrowserContext() const {
  return browsing_instance_->browser_context();
}

/*static*/
SiteInstance* SiteInstance::Create(BrowserContext* browser_context) {
  return new SiteInstanceImpl(new BrowsingInstance(browser_context));
}

/*static*/
SiteInstance* SiteInstance::CreateForURL(BrowserContext* browser_context,
                                         const GURL& url) {
  // This BrowsingInstance may be deleted if it returns an existing
  // SiteInstance.
  scoped_refptr<BrowsingInstance> instance(
      new BrowsingInstance(browser_context));
  return instance->GetSiteInstanceForURL(url);
}

/*static*/
bool SiteInstance::IsSameWebSite(BrowserContext* browser_context,
                                 const GURL& real_src_url,
                                 const GURL& real_dest_url) {
  GURL src_url = SiteInstanceImpl::GetEffectiveURL(browser_context,
                                                   real_src_url);
  GURL dest_url = SiteInstanceImpl::GetEffectiveURL(browser_context,
                                                    real_dest_url);

  // We infer web site boundaries based on the registered domain name of the
  // top-level page and the scheme.  We do not pay attention to the port if
  // one is present, because pages served from different ports can still
  // access each other if they change their document.domain variable.

  // Some special URLs will match the site instance of any other URL. This is
  // done before checking both of them for validity, since we want these URLs
  // to have the same site instance as even an invalid one.
  if (IsRendererDebugURL(src_url) || IsRendererDebugURL(dest_url))
    return true;

  // If either URL is invalid, they aren't part of the same site.
  if (!src_url.is_valid() || !dest_url.is_valid())
    return false;

  // If the destination url is just a blank page, we treat them as part of the
  // same site.
  GURL blank_page(url::kAboutBlankURL);
  if (dest_url == blank_page)
    return true;

  // If the schemes differ, they aren't part of the same site.
  if (src_url.scheme() != dest_url.scheme())
    return false;

  return net::registry_controlled_domains::SameDomainOrHost(
      src_url,
      dest_url,
      net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
}

/*static*/
GURL SiteInstance::GetSiteForURL(BrowserContext* browser_context,
                                 const GURL& real_url) {
  // TODO(fsamuel, creis): For some reason appID is not recognized as a host.
  if (real_url.SchemeIs(kGuestScheme))
    return real_url;

  GURL url = SiteInstanceImpl::GetEffectiveURL(browser_context, real_url);

  // URLs with no host should have an empty site.
  GURL site;

  // TODO(creis): For many protocols, we should just treat the scheme as the
  // site, since there is no host.  e.g., file:, about:, chrome:

  // If the url has a host, then determine the site.
  if (url.has_host()) {
    // Only keep the scheme and registered domain as given by GetOrigin.  This
    // may also include a port, which we need to drop.
    site = url.GetOrigin();

    // Remove port, if any.
    if (site.has_port()) {
      GURL::Replacements rep;
      rep.ClearPort();
      site = site.ReplaceComponents(rep);
    }

    // If this URL has a registered domain, we only want to remember that part.
    std::string domain =
        net::registry_controlled_domains::GetDomainAndRegistry(
            url,
            net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
    if (!domain.empty()) {
      GURL::Replacements rep;
      rep.SetHostStr(domain);
      site = site.ReplaceComponents(rep);
    }
  }
  return site;
}

/*static*/
GURL SiteInstanceImpl::GetEffectiveURL(BrowserContext* browser_context,
                                       const GURL& url) {
  return GetContentClient()->browser()->
      GetEffectiveURL(browser_context, url);
}

void SiteInstanceImpl::RenderProcessHostDestroyed(RenderProcessHost* host) {
  DCHECK_EQ(process_, host);
  process_->RemoveObserver(this);
  process_ = NULL;
}

void SiteInstanceImpl::LockToOrigin() {
  // We currently only restrict this process to a particular site if the
  // --enable-strict-site-isolation or --site-per-process flags are present.
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  if (command_line.HasSwitch(switches::kEnableStrictSiteIsolation) ||
      command_line.HasSwitch(switches::kSitePerProcess)) {
    ChildProcessSecurityPolicyImpl* policy =
        ChildProcessSecurityPolicyImpl::GetInstance();
    policy->LockToOrigin(process_->GetID(), site_);
  }
}

}  // namespace content
