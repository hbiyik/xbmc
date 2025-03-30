/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DRMUtils.h"

#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/DRMHelpers.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"

#include "PlatformDefs.h"

using namespace KODI::WINDOWING::GBM;

namespace
{
const std::string SETTING_VIDEOSCREEN_LIMITGUISIZE = "videoscreen.limitguisize";
const std::string SETTING_VIDEOPLAYER_DRM10BITMODE = "videoplayer.drm10bitmode";

void DrmFbDestroyCallback(gbm_bo* bo, void* data)
{
  drm_fb* fb = static_cast<drm_fb*>(data);

  if (fb->fb_id > 0)
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - removing framebuffer: {}", __FUNCTION__, fb->fb_id);
    int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
    drmModeRmFB(drm_fd, fb->fb_id);
  }

  delete fb;
}
}

CDRMUtils::~CDRMUtils()
{
  DestroyDrm();
}

bool CDRMUtils::SetMode(const RESOLUTION_INFO& res)
{
  if (!m_connector->CheckConnector())
    return false;

  m_mode = m_connector->GetModeForIndex(std::atoi(res.strId.c_str()));
  m_width = res.iWidth;
  m_height = res.iHeight;

  CLog::Log(LOGDEBUG, "CDRMUtils::{} - found crtc mode: {}x{}{} @ {} Hz", __FUNCTION__,
            m_mode->hdisplay, m_mode->vdisplay, m_mode->flags & DRM_MODE_FLAG_INTERLACE ? "i" : "",
            m_mode->vrefresh);

  return true;
}

drm_fb * CDRMUtils::DrmFbGetFromBo(struct gbm_bo *bo)
{
  {
    struct drm_fb *fb = static_cast<drm_fb *>(gbm_bo_get_user_data(bo));
    if(fb)
        return fb;
  }

  struct drm_fb *fb = new drm_fb;
  fb->bo = bo;
  fb->format = m_gui_plane->GetFormat();

  uint32_t width,
           height,
           handles[4] = {0},
           strides[4] = {0},
           offsets[4] = {0};

  uint64_t modifiers[4] = {0};

  width = gbm_bo_get_width(bo);
  height = gbm_bo_get_height(bo);

#if defined(HAS_GBM_MODIFIERS)
  for (int i = 0; i < gbm_bo_get_plane_count(bo); i++)
  {
    handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
    strides[i] = gbm_bo_get_stride_for_plane(bo, i);
    offsets[i] = gbm_bo_get_offset(bo, i);
    modifiers[i] = gbm_bo_get_modifier(bo);
  }
#else
  handles[0] = gbm_bo_get_handle(bo).u32;
  strides[0] = gbm_bo_get_stride(bo);
  memset(offsets, 0, 16);
#endif

  uint32_t flags = 0;

  if (modifiers[0] && modifiers[0] != DRM_FORMAT_MOD_INVALID)
  {
    flags |= DRM_MODE_FB_MODIFIERS;
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - using modifier: {}", __FUNCTION__,
              DRMHELPERS::ModifierToString(modifiers[0]));
  }

  int ret = drmModeAddFB2WithModifiers(m_fd,
                                       width,
                                       height,
                                       fb->format,
                                       handles,
                                       strides,
                                       offsets,
                                       modifiers,
                                       &fb->fb_id,
                                       flags);

  if(ret < 0)
  {
    ret = drmModeAddFB2(m_fd,
                        width,
                        height,
                        fb->format,
                        handles,
                        strides,
                        offsets,
                        &fb->fb_id,
                        flags);

    if (ret < 0)
    {
      delete (fb);
      CLog::Log(LOGDEBUG, "CDRMUtils::{} - failed to add framebuffer: {} ({})", __FUNCTION__, strerror(errno), errno);
      return nullptr;
    }
  }

  gbm_bo_set_user_data(bo, fb, DrmFbDestroyCallback);

  return fb;
}

bool CDRMUtils::FindPreferredMode()
{
  if (m_mode)
    return true;

  for (int i = 0, area = 0; i < m_connector->GetModesCount(); i++)
  {
    drmModeModeInfo* current_mode = m_connector->GetModeForIndex(i);

    if(current_mode->type & DRM_MODE_TYPE_PREFERRED)
    {
      m_mode = current_mode;
      CLog::Log(LOGDEBUG, "CDRMUtils::{} - found preferred mode: {}x{}{} @ {} Hz", __FUNCTION__,
                m_mode->hdisplay, m_mode->vdisplay,
                m_mode->flags & DRM_MODE_FLAG_INTERLACE ? "i" : "", m_mode->vrefresh);
      break;
    }

    auto current_area = current_mode->hdisplay * current_mode->vdisplay;
    if (current_area > area)
    {
      m_mode = current_mode;
      area = current_area;
    }
  }

  if(!m_mode)
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - failed to find preferred mode", __FUNCTION__);
    return false;
  }

  return true;
}

// finds a video plane with given format + modifier, and a gui plane with inited format + modifier for any available crtcs
bool CDRMUtils::FindPlanes(uint32_t format, uint64_t modifier) {
    // current config already satisfies
    if (m_gui_plane != nullptr && m_video_plane != nullptr
            && m_video_plane->SupportsFormatAndModifier(format, modifier))
        return true;

    uint32_t guiformat = m_gui_plane->GetFormat();
    // loop over current crtc which is capable of rendering on the connected encoder (port)
    for (size_t crtc_offset = 0; crtc_offset < m_crtcs.size(); crtc_offset++) {
        if (!(m_encoder->GetPossibleCrtcs() & (1 << crtc_offset)))
            continue;
        // loop for each gui plane candidate which satisfies the current EGL rendered format
        // gui format is decided in FindGuiPlane
        for (auto &gui_plane : m_planes) {
            auto gplane = gui_plane.get();
            if (!(gplane->GetPossibleCrtcs() & (1 << crtc_offset))
                    || !gplane->SupportsFormatAndModifier(guiformat, DRM_FORMAT_MOD_LINEAR))
                continue;
            // loop for each format satisfying video plane candidate which is different than gui plane candidate
            for (auto &vid_plane : m_planes) {
                auto vplane = vid_plane.get();
                if (!(vplane->GetPossibleCrtcs() & (1 << crtc_offset))
                        || !vplane->SupportsFormatAndModifier(format, modifier)
                        || vplane->GetId() == gplane->GetId())
                    continue;
                bool zpos_available = vplane->SupportsProperty("zpos") && gplane->SupportsProperty("zpos");
                bool zpos_mutable = zpos_available && !vplane->IsPropertyImmutable("zpos").value() && !gplane->IsPropertyImmutable("zpos").value();
                // if zpos is supported, select the planes, and put video on top and set as current crtc
                if (zpos_available) {
                    uint64_t zpos_gui = gplane->GetRangePropertyValue("zpos");
                    uint64_t zpos_vid = vplane->GetRangePropertyValue("zpos");
                    // zpos is immutable, make sure gui is on top by selecting correct zpos
                    if (!zpos_mutable && zpos_gui <= zpos_vid)
                        continue;
                    // zpos is mutable, make sure gui is on top by setting correct zpos
                    if(zpos_mutable && zpos_gui <= zpos_vid) {
                        gplane->SetProperty("zpos", 1);
                        vplane->SetProperty("zpos", 0);
                    }
                    m_crtc = m_crtcs[crtc_offset].get();
                    m_gui_plane = gplane;
                    m_video_plane = vplane;
                    goto success;
                // if zpos is not supported, make sure video plane id > gui plane id
                // this is how drm sorts when zpos is not available. Set crtc as the current crtc
                } else if (vplane->GetId() < gplane->GetId()) {
                    m_crtc = m_crtcs[crtc_offset].get();
                    m_gui_plane = gplane;
                    m_video_plane = vplane;
                    goto success;
                }
            }
        }
    }

    CLog::Log(LOGERROR, "CDRMUtils::{} - Can not find a Video Plane plane with format {}, modifier {}. Re-initing",
            __FUNCTION__, DRMHELPERS::FourCCToString(format), DRMHELPERS::ModifierToString(modifier));
    return InitGuiPlane(nullptr, 0);

success:
    m_gui_plane->SetFormat(guiformat);
    CLog::Log(LOGINFO, "CDRMUtils::{} - Switched GUI Plane to id:{}, video plane to id:{} on crtc id:{} video for format:{}, modifier:{}",
            __FUNCTION__, m_gui_plane->GetId(), m_video_plane->GetId(), m_crtc->GetId(),
            DRMHELPERS::FourCCToString(format), DRMHELPERS::ModifierToString(modifier));
    return true;
}

// determines the GUI rendering format and selects a plane+crtc for it without considering the future video plane
bool CDRMUtils::InitGuiPlane(CEGLContextUtils* eglContext, EGLint renderableType) {
    int mode = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(SETTING_VIDEOPLAYER_DRM10BITMODE);
    std::vector<std::unique_ptr<CDRMPlane>> gui_candidates;

    m_gui_plane = nullptr;
    m_video_plane = nullptr;
    m_crtc = nullptr;
    std::map<std::uint32_t, std::vector<uint32_t>> formats{{DRM_FORMAT_ARGB2101010, {DRM_FORMAT_XRGB2101010, DRM_FORMAT_ARGB2101010}},
                                                           {DRM_FORMAT_ARGB8888, {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888}},};

    for (auto const& format : formats) {
        // check if 8bit is forced
        if (mode && format.first == DRM_FORMAT_ARGB2101010)
            continue;

        // log if 8bit mode fallback
        if (!mode && format.first != DRM_FORMAT_ARGB2101010)
            CLog::Log(LOGWARNING, "CDRMUtils::{} - Requested 10bit GUI or EGL format support is not found, falling back to 8 bit",
                    __FUNCTION__);

        // check if EGL supports a format compatible with the DRM format
        uint32_t eglformat = DRM_FORMAT_INVALID;
        if(eglContext){
            for (uint32_t eformat : format.second){
                if (!eglContext->ChooseConfig(renderableType, eformat))
                    continue;
                eglformat = eformat;
            }
            if(eglformat == DRM_FORMAT_INVALID){
                CLog::Log(LOGWARNING, "CDRMUtils::{} - No egl format found for plane format {}",
                                    __FUNCTION__, DRMHELPERS::FourCCToString(format.first));
                continue;
            }
        }

        // loop through crtcs connected to encoder (port)
        for (size_t crtc_offset = 0; crtc_offset < m_crtcs.size(); crtc_offset++) {
            if (!(m_encoder->GetPossibleCrtcs() & (1 << crtc_offset)))
                continue;

            // find a plane satisfies the format and crtc
            auto guiPlane = std::find_if(m_planes.begin(), m_planes.end(), [&crtc_offset, &format](auto &plane) {
                if (plane->GetPossibleCrtcs() & (1 << crtc_offset))
                    return (plane->SupportsFormatAndModifier(format.first, DRM_FORMAT_MOD_LINEAR));
                return false;
            });
            if (guiPlane == m_planes.end())
                continue;

            m_crtc = m_crtcs[crtc_offset].get();
            m_gui_plane = guiPlane->get();
            CLog::Log(LOGINFO, "CDRMUtils::{} - Requested GUI plane is found with id: {} and plane format {}, egl format over crtc id: {}",
                    __FUNCTION__, m_gui_plane->GetId(), DRMHELPERS::FourCCToString(format.first), DRMHELPERS::FourCCToString(eglformat),
                    m_crtc->GetId());
            m_gui_plane->SetFormat(format.first);
            return true;
        }
    }

    CLog::Log(LOGERROR, "CDRMUtils::{} - No 10bit nor 8bit capable GUI plane found", __FUNCTION__);
    return false;
}


void CDRMUtils::PrintDrmDeviceInfo(drmDevicePtr device)
{
  std::string message;

  // clang-format off
  message.append(fmt::format("CDRMUtils::{} - DRM Device Info:", __FUNCTION__));
  message.append(fmt::format("\n  available_nodes: {:#04x}", device->available_nodes));
  message.append("\n  nodes:");

  for (int i = 0; i < DRM_NODE_MAX; i++)
  {
    if (device->available_nodes & 1 << i)
      message.append(fmt::format("\n    nodes[{}]: {}", i, device->nodes[i]));
  }

  message.append(fmt::format("\n  bustype: {:#04x}", device->bustype));

  if (device->bustype == DRM_BUS_PCI)
  {
    message.append("\n    pci:");
    message.append(fmt::format("\n      domain: {:#04x}", device->businfo.pci->domain));
    message.append(fmt::format("\n      bus:    {:#02x}", device->businfo.pci->bus));
    message.append(fmt::format("\n      dev:    {:#02x}", device->businfo.pci->dev));
    message.append(fmt::format("\n      func:   {:#1}", device->businfo.pci->func));

    message.append("\n  deviceinfo:");
    message.append("\n    pci:");
    message.append(fmt::format("\n      vendor_id:    {:#04x}", device->deviceinfo.pci->vendor_id));
    message.append(fmt::format("\n      device_id:    {:#04x}", device->deviceinfo.pci->device_id));
    message.append(fmt::format("\n      subvendor_id: {:#04x}", device->deviceinfo.pci->subvendor_id));
    message.append(fmt::format("\n      subdevice_id: {:#04x}", device->deviceinfo.pci->subdevice_id));
  }
  else if (device->bustype == DRM_BUS_USB)
  {
    message.append("\n    usb:");
    message.append(fmt::format("\n      bus: {:#03x}", device->businfo.usb->bus));
    message.append(fmt::format("\n      dev: {:#03x}", device->businfo.usb->dev));

    message.append("\n  deviceinfo:");
    message.append("\n    usb:");
    message.append(fmt::format("\n      vendor:  {:#04x}", device->deviceinfo.usb->vendor));
    message.append(fmt::format("\n      product: {:#04x}", device->deviceinfo.usb->product));
  }
  else if (device->bustype == DRM_BUS_PLATFORM)
  {
    message.append("\n    platform:");
    message.append(fmt::format("\n      fullname: {}", device->businfo.platform->fullname));
  }
  else if (device->bustype == DRM_BUS_HOST1X)
  {
    message.append("\n    host1x:");
    message.append(fmt::format("\n      fullname: {}", device->businfo.host1x->fullname));
  }
  else
    message.append("\n    unhandled bus type");
  // clang-format on

  CLog::Log(LOGDEBUG, "{}", message);
}

bool CDRMUtils::OpenDrm(bool needConnector)
{
  int numDevices = drmGetDevices2(0, nullptr, 0);
  if (numDevices <= 0)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - no drm devices found: ({})", __FUNCTION__,
              strerror(errno));
    return false;
  }

  CLog::Log(LOGDEBUG, "CDRMUtils::{} - drm devices found: {}", __FUNCTION__, numDevices);

  std::vector<drmDevicePtr> devices(numDevices);

  int ret = drmGetDevices2(0, devices.data(), devices.size());
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - drmGetDevices2 return an error: ({})", __FUNCTION__,
              strerror(errno));
    return false;
  }

  for (const auto device : devices)
  {
    if (!(device->available_nodes & 1 << DRM_NODE_PRIMARY))
      continue;

    close(m_fd);
    m_fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
    if (m_fd < 0)
      continue;

    if (needConnector)
    {
      auto resources = drmModeGetResources(m_fd);
      if (!resources)
        continue;

      m_connectors.clear();
      for (int i = 0; i < resources->count_connectors; i++)
        m_connectors.emplace_back(std::make_unique<CDRMConnector>(m_fd, resources->connectors[i]));

      drmModeFreeResources(resources);

      if (!FindConnector())
        continue;
    }

    CLog::Log(LOGDEBUG, "CDRMUtils::{} - opened device: {}", __FUNCTION__,
              device->nodes[DRM_NODE_PRIMARY]);

    PrintDrmDeviceInfo(device);

    const char* renderPath = drmGetRenderDeviceNameFromFd(m_fd);

    if (!renderPath)
      renderPath = drmGetDeviceNameFromFd2(m_fd);

    if (!renderPath)
      renderPath = drmGetDeviceNameFromFd(m_fd);

    if (renderPath)
    {
      m_renderDevicePath = renderPath;
      m_renderFd = open(renderPath, O_RDWR | O_CLOEXEC);
      if (m_renderFd != 0)
        CLog::Log(LOGDEBUG, "CDRMUtils::{} - opened render node: {}", __FUNCTION__, renderPath);
    }

    drmFreeDevices(devices.data(), devices.size());
    return true;
  }

  drmFreeDevices(devices.data(), devices.size());
  return false;
}

bool CDRMUtils::InitDrm()
{
  if (m_fd < 0)
    return false;

  /* caps need to be set before allocating connectors, encoders, crtcs, and planes */
  int ret = drmSetClientCap(m_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (ret)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - failed to set universal planes capability: {}",
              __FUNCTION__, strerror(errno));
    return false;
  }

  ret = drmSetClientCap(m_fd, DRM_CLIENT_CAP_STEREO_3D, 1);
  if (ret)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - failed to set stereo 3d capability: {}", __FUNCTION__,
              strerror(errno));
    return false;
  }

#if defined(DRM_CLIENT_CAP_ASPECT_RATIO)
  ret = drmSetClientCap(m_fd, DRM_CLIENT_CAP_ASPECT_RATIO, 0);
  if (ret != 0)
    CLog::Log(LOGERROR, "CDRMUtils::{} - aspect ratio capability is not supported: {}",
              __FUNCTION__, strerror(errno));
#endif

  auto resources = drmModeGetResources(m_fd);
  if (!resources)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - failed to get drm resources: {}", __FUNCTION__, strerror(errno));
    return false;
  }

  m_connectors.clear();
  for (int i = 0; i < resources->count_connectors; i++)
    m_connectors.emplace_back(std::make_unique<CDRMConnector>(m_fd, resources->connectors[i]));

  m_encoders.clear();
  for (int i = 0; i < resources->count_encoders; i++)
    m_encoders.emplace_back(std::make_unique<CDRMEncoder>(m_fd, resources->encoders[i]));

  m_crtcs.clear();
  for (int i = 0; i < resources->count_crtcs; i++)
    m_crtcs.emplace_back(std::make_unique<CDRMCrtc>(m_fd, resources->crtcs[i]));

  drmModeFreeResources(resources);

  auto planeResources = drmModeGetPlaneResources(m_fd);
  if (!planeResources)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - failed to get drm plane resources: {}", __FUNCTION__, strerror(errno));
    return false;
  }

  m_planes.clear();
  for (uint32_t i = 0; i < planeResources->count_planes; i++)
  {
    m_planes.emplace_back(std::make_unique<CDRMPlane>(m_fd, planeResources->planes[i]));
    m_planes[i]->FindModifiers();
  }

  drmModeFreePlaneResources(planeResources);

  if (!FindConnector())
    return false;

  if (!FindEncoder())
    return false;

  if (!FindCrtc())
    return false;

  if (!FindPreferredMode())
    return false;

  ret = drmSetMaster(m_fd);
  if (ret < 0)
  {
    CLog::Log(LOGDEBUG,
              "CDRMUtils::{} - failed to set drm master, will try to authorize instead: {}",
              __FUNCTION__, strerror(errno));

    drm_magic_t magic;

    ret = drmGetMagic(m_fd, &magic);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "CDRMUtils::{} - failed to get drm magic: {}", __FUNCTION__,
                strerror(errno));
      return false;
    }

    ret = drmAuthMagic(m_fd, magic);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "CDRMUtils::{} - failed to authorize drm magic: {}", __FUNCTION__,
                strerror(errno));
      return false;
    }

    CLog::Log(LOGINFO, "CDRMUtils::{} - successfully authorized drm magic", __FUNCTION__);
  }

  return true;
}

bool CDRMUtils::FindConnector()
{
  auto settingsComponent = CServiceBroker::GetSettingsComponent();
  if (!settingsComponent)
    return false;

  auto settings = settingsComponent->GetSettings();
  if (!settings)
    return false;

  std::vector<std::unique_ptr<CDRMConnector>>::iterator connector;

  std::string connectorName = settings->GetString(CSettings::SETTING_VIDEOSCREEN_MONITOR);
  if (connectorName != "Default")
  {
    connector = std::find_if(m_connectors.begin(), m_connectors.end(),
                             [&connectorName](auto& connector)
                             {
                               return connector->GetEncoderId() > 0 && connector->IsConnected() &&
                                      connector->GetName() == connectorName;
                             });

    if (connector == m_connectors.end())
    {
      CLog::Log(LOGDEBUG, "CDRMUtils::{} - failed to find specified connector: {}, trying default",
                __FUNCTION__, connectorName);
      connectorName = "Default";
    }
  }

  if (connectorName == "Default")
  {
    connector = std::find_if(m_connectors.begin(), m_connectors.end(),
                             [](auto& connector)
                             { return connector->GetEncoderId() > 0 && connector->IsConnected(); });
  }

  if (connector == m_connectors.end())
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - failed to find connected connector", __FUNCTION__);
    return false;
  }

  CLog::Log(LOGINFO, "CDRMUtils::{} - using connector: {}", __FUNCTION__,
            connector->get()->GetName());

  m_connector = connector->get();
  return true;
}

bool CDRMUtils::FindEncoder()
{
  auto encoder = std::find_if(m_encoders.begin(), m_encoders.end(), [this](auto& encoder) {
    return encoder->GetEncoderId() == m_connector->GetEncoderId();
  });

  if (encoder == m_encoders.end())
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - failed to find encoder for connector id: {}", __FUNCTION__,
              *m_connector->GetConnectorId());
    return false;
  }

  CLog::Log(LOGINFO, "CDRMUtils::{} - using encoder: {}", __FUNCTION__,
            encoder->get()->GetEncoderId());

  m_encoder = encoder->get();
  return true;
}

bool CDRMUtils::FindCrtc()
{
  for (size_t i = 0; i < m_crtcs.size(); i++)
  {
    if (m_encoder->GetPossibleCrtcs() & (1 << i))
    {
      if (m_crtcs[i]->GetCrtcId() == m_encoder->GetCrtcId())
      {
        m_orig_crtc = m_crtcs[i].get();
        if (m_orig_crtc->GetModeValid())
        {
          m_mode = m_orig_crtc->GetMode();
          CLog::Log(LOGDEBUG, "CDRMUtils::{} - original crtc mode: {}x{}{} @ {} Hz", __FUNCTION__,
                    m_mode->hdisplay, m_mode->vdisplay,
                    m_mode->flags & DRM_MODE_FLAG_INTERLACE ? "i" : "", m_mode->vrefresh);
        }
        return true;
      }
    }
  }

  return false;
}

bool CDRMUtils::RestoreOriginalMode()
{
  if(!m_orig_crtc)
  {
    return false;
  }

  auto ret = drmModeSetCrtc(m_fd, m_orig_crtc->GetCrtcId(), m_orig_crtc->GetBufferId(),
                            m_orig_crtc->GetX(), m_orig_crtc->GetY(), m_connector->GetConnectorId(),
                            1, m_orig_crtc->GetMode());

  if(ret)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - failed to set original crtc mode", __FUNCTION__);
    return false;
  }

  CLog::Log(LOGDEBUG, "CDRMUtils::{} - set original crtc mode", __FUNCTION__);

  return true;
}

void CDRMUtils::DestroyDrm()
{
  RestoreOriginalMode();

  if (drmAuthMagic(m_fd, 0) == EINVAL)
    drmDropMaster(m_fd);

  close(m_renderFd);
  close(m_fd);

  m_connector = nullptr;
  m_encoder = nullptr;
  m_crtc = nullptr;
  m_orig_crtc = nullptr;
  m_video_plane = nullptr;
  m_gui_plane = nullptr;
}

RESOLUTION_INFO CDRMUtils::GetResolutionInfo(drmModeModeInfoPtr mode)
{
  RESOLUTION_INFO res;
  res.iScreenWidth = mode->hdisplay;
  res.iScreenHeight = mode->vdisplay;
  res.iWidth = res.iScreenWidth;
  res.iHeight = res.iScreenHeight;

  int limit = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      SETTING_VIDEOSCREEN_LIMITGUISIZE);
  if (limit > 0 && res.iScreenWidth > 1920 && res.iScreenHeight > 1080)
  {
    switch (limit)
    {
      case 1: // 720p
        res.iWidth = 1280;
        res.iHeight = 720;
        break;
      case 2: // 1080p / 720p (>30hz)
        res.iWidth = mode->vrefresh > 30 ? 1280 : 1920;
        res.iHeight = mode->vrefresh > 30 ? 720 : 1080;
        break;
      case 3: // 1080p
        res.iWidth = 1920;
        res.iHeight = 1080;
        break;
      case 4: // Unlimited / 1080p (>30hz)
        res.iWidth = mode->vrefresh > 30 ? 1920 : res.iScreenWidth;
        res.iHeight = mode->vrefresh > 30 ? 1080 : res.iScreenHeight;
        break;
    }
  }

  if (mode->clock % 5 != 0)
    res.fRefreshRate = static_cast<float>(mode->vrefresh) * (1000.0f/1001.0f);
  else
    res.fRefreshRate = mode->vrefresh;
  res.iSubtitles = res.iHeight;
  res.fPixelRatio = 1.0f;
  res.bFullScreen = true;

  if (mode->flags & DRM_MODE_FLAG_3D_MASK)
  {
    if (mode->flags & DRM_MODE_FLAG_3D_TOP_AND_BOTTOM)
      res.dwFlags = D3DPRESENTFLAG_MODE3DTB;
    else if (mode->flags & DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF)
      res.dwFlags = D3DPRESENTFLAG_MODE3DSBS;
  }
  else if (mode->flags & DRM_MODE_FLAG_INTERLACE)
    res.dwFlags = D3DPRESENTFLAG_INTERLACED;
  else
    res.dwFlags = D3DPRESENTFLAG_PROGRESSIVE;

  res.strMode =
      StringUtils::Format("{}x{}{} @ {:.6f} Hz", res.iScreenWidth, res.iScreenHeight,
                          res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "", res.fRefreshRate);
  return res;
}

RESOLUTION_INFO CDRMUtils::GetCurrentMode()
{
  return GetResolutionInfo(m_mode);
}

std::vector<RESOLUTION_INFO> CDRMUtils::GetModes()
{
  std::vector<RESOLUTION_INFO> resolutions;
  resolutions.reserve(m_connector->GetModesCount());

  for (auto i = 0; i < m_connector->GetModesCount(); i++)
  {
    RESOLUTION_INFO res = GetResolutionInfo(m_connector->GetModeForIndex(i));
    res.strId = std::to_string(i);
    resolutions.push_back(res);
  }

  return resolutions;
}

std::vector<std::string> CDRMUtils::GetConnectedConnectorNames()
{
  std::vector<std::string> connectorNames;
  for (const auto& connector : m_connectors)
  {
    if (connector->IsConnected())
      connectorNames.emplace_back(connector->GetName());
  }

  return connectorNames;
}
