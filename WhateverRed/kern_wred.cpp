//
//  kern_weg.cpp
//  WhateverRed
//
//  Copyright © 2017 vit9696. All rights reserved.
//  Copyright © 2022 ChefKiss Inc. All rights reserved.
//

#include "kern_wred.hpp"
#include <Headers/kern_api.hpp>
#include <Headers/kern_cpu.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_iokit.hpp>
#include <IOKit/graphics/IOFramebuffer.h>

// This is a hack to let us access protected properties.
struct FramebufferViewer : public IOFramebuffer {
    static IOMemoryMap *&getVramMap(IOFramebuffer *fb) { return static_cast<FramebufferViewer *>(fb)->fVramMap; }
};

static const char *pathIOGraphics[] = {"/System/Library/Extensions/IOGraphicsFamily.kext/IOGraphicsFamily"};
static const char *pathAGDPolicy[] = {"/System/Library/Extensions/AppleGraphicsControl.kext/Contents/PlugIns/"
                                      "AppleGraphicsDevicePolicy.kext/Contents/MacOS/AppleGraphicsDevicePolicy"};

static KernelPatcher::KextInfo kextIOGraphics {
    "com.apple.iokit.IOGraphicsFamily",
    pathIOGraphics,
    arrsize(pathIOGraphics),
    {true},
    {},
    KernelPatcher::KextInfo::Unloaded,
};
static KernelPatcher::KextInfo kextAGDPolicy {
    "com.apple.driver.AppleGraphicsDevicePolicy",
    pathAGDPolicy,
    arrsize(pathAGDPolicy),
    {true},
    {},
    KernelPatcher::KextInfo::Unloaded,
};

WRed *WRed::callbackWRED = nullptr;

void WRed::init() {
    callbackWRED = this;

    // Background init fix is only necessary on 10.10 and newer.
    // Former boot-arg name is igfxrst.
    if (getKernelVersion() >= KernelVersion::Yosemite) {
        PE_parse_boot_argn("gfxrst", &resetFramebuffer, sizeof(resetFramebuffer));
        if (resetFramebuffer >= FB_TOTAL) {
            SYSLOG("wred", "invalid igfxrset value %d, falling back to autodetect", resetFramebuffer);
            resetFramebuffer = FB_DETECT;
        }
    } else {
        resetFramebuffer = FB_NONE;
    }

    // Black screen fix is needed everywhere, but the form depends on the
    // boot-arg. Former boot-arg name is ngfxpatch.
    char agdp[128];
    if (PE_parse_boot_argn("agdpmod", agdp, sizeof(agdp))) processGraphicsPolicyStr(agdp);

    // Callback setup is only done here for compatibility.
    lilu.onPatcherLoadForce(
        [](void *user, KernelPatcher &patcher) { static_cast<WRed *>(user)->processKernel(patcher); }, this);

    lilu.onKextLoadForce(
        nullptr, 0,
        [](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
            static_cast<WRed *>(user)->processKext(patcher, index, address, size);
        },
        this);

    // Perform a background fix.
    if (resetFramebuffer != FB_NONE) lilu.onKextLoadForce(&kextIOGraphics);

    // Perform a black screen fix.
    if (graphicsDisplayPolicyMod != AGDP_NONE_SET) lilu.onKextLoad(&kextAGDPolicy);

    rad.init();
}

void WRed::deinit() { rad.deinit(); }

void WRed::processKernel(KernelPatcher &patcher) {
    // Correct GPU properties
    auto devInfo = DeviceInfo::create();
    if (devInfo) {
        devInfo->processSwitchOff();

        if (graphicsDisplayPolicyMod == AGDP_DETECT) { /* Default detect only */
            auto getAgpdMod = [this](IORegistryEntry *device) {
                auto prop = device->getProperty("agdpmod");
                if (prop) {
                    DBGLOG("wred", "found agdpmod in external GPU %s", safeString(device->getName()));
                    const char *agdp = nullptr;
                    auto propStr = OSDynamicCast(OSString, prop);
                    auto propData = OSDynamicCast(OSData, prop);
                    if (propStr) {
                        agdp = propStr->getCStringNoCopy();
                    } else if (propData && propData->getLength() > 0) {
                        agdp = static_cast<const char *>(propData->getBytesNoCopy());
                        if (agdp && agdp[propData->getLength() - 1] != '\0') {
                            DBGLOG("wred", "agdpmod config is not null terminated");
                            agdp = nullptr;
                        }
                    }
                    if (agdp) {
                        processGraphicsPolicyStr(agdp);
                        return true;
                    }
                }

                return false;
            };

            size_t extNum = devInfo->videoExternal.size();
            for (size_t i = 0; i < extNum; i++) {
                if (getAgpdMod(devInfo->videoExternal[i].video)) break;
            }
            if (devInfo->videoBuiltin != nullptr && graphicsDisplayPolicyMod == AGDP_DETECT) /* Default detect only */
                getAgpdMod(devInfo->videoBuiltin);
        }

        rad.processKernel(patcher);

        DeviceInfo::deleter(devInfo);
    }

    // Disable mods that did not find a way to function.
    if (resetFramebuffer == FB_DETECT) {
        resetFramebuffer = FB_NONE;
        kextIOGraphics.switchOff();
    }

    if ((graphicsDisplayPolicyMod & AGDP_DETECT) || graphicsDisplayPolicyMod == AGDP_NONE_SET) {
        graphicsDisplayPolicyMod = AGDP_NONE_SET;
        kextAGDPolicy.switchOff();
    }

    // We need to load vinfo for cleanup and copy.
    if (resetFramebuffer == FB_COPY || resetFramebuffer == FB_ZEROFILL) {
        auto info = reinterpret_cast<vc_info *>(patcher.solveSymbol(KernelPatcher::KernelID, "_vinfo"));
        if (info) {
            consoleVinfo = *info;
            DBGLOG("wred", "vinfo 1: %u:%u %u:%u:%u", consoleVinfo.v_height, consoleVinfo.v_width, consoleVinfo.v_depth,
                consoleVinfo.v_rowbytes, consoleVinfo.v_type);
            DBGLOG("wred", "vinfo 2: %s %u:%u %u:%u:%u", consoleVinfo.v_name, consoleVinfo.v_rows,
                consoleVinfo.v_columns, consoleVinfo.v_rowscanbytes, consoleVinfo.v_scale, consoleVinfo.v_rotate);
            gotConsoleVinfo = true;
        } else {
            SYSLOG("wred", "failed to obtain vcinfo");
            patcher.clearError();
        }
    }
}

void WRed::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
    if (kextIOGraphics.loadIndex == index) {
        gIOFBVerboseBootPtr = patcher.solveSymbol<uint8_t *>(index, "__ZL16gIOFBVerboseBoot", address, size);
        if (gIOFBVerboseBootPtr) {
            KernelPatcher::RouteRequest request("__ZN13IOFramebuffer6initFBEv", wrapFramebufferInit,
                orgFramebufferInit);
            patcher.routeMultiple(index, &request, 1, address, size);
        } else {
            SYSLOG("wred", "failed to resolve gIOFBVerboseBoot");
            patcher.clearError();
        }
        return;
    } else if (kextAGDPolicy.loadIndex == index) {
        processGraphicsPolicyMods(patcher, address, size);
        return;
    }

    if (rad.processKext(patcher, index, address, size)) return;
}

void WRed::processGraphicsPolicyStr(const char *agdp) {
    DBGLOG("wred", "agdpmod using config %s", agdp);
    if (strstr(agdp, "detect")) {
        graphicsDisplayPolicyMod = AGDP_DETECT_SET;
    } else if (strstr(agdp, "ignore")) {
        graphicsDisplayPolicyMod = AGDP_NONE_SET;
    } else {
        graphicsDisplayPolicyMod = AGDP_NONE_SET;
        if (strstr(agdp, "vit9696")) graphicsDisplayPolicyMod |= AGDP_VIT9696;
        if (strstr(agdp, "pikera")) graphicsDisplayPolicyMod |= AGDP_PIKERA;
        if (strstr(agdp, "cfgmap")) graphicsDisplayPolicyMod |= AGDP_CFGMAP;
    }
}

void WRed::processGraphicsPolicyMods(KernelPatcher &patcher, mach_vm_address_t address, size_t size) {
    if (graphicsDisplayPolicyMod & AGDP_VIT9696) {
        uint8_t find[] = {0xBA, 0x05, 0x00, 0x00, 0x00};
        uint8_t replace[] = {0xBA, 0x00, 0x00, 0x00, 0x00};
        KernelPatcher::LookupPatch patch {&kextAGDPolicy, find, replace, sizeof(find), 1};

        patcher.applyLookupPatch(&patch);
        if (patcher.getError() != KernelPatcher::Error::NoError) {
            SYSLOG("wred", "failed to apply agdp vit9696's patch %d", patcher.getError());
            patcher.clearError();
        }
    }

    if (graphicsDisplayPolicyMod & AGDP_PIKERA) {
        KernelPatcher::LookupPatch patch {&kextAGDPolicy, reinterpret_cast<const uint8_t *>("board-id"),
            reinterpret_cast<const uint8_t *>("board-ix"), sizeof("board-id"), 1};

        patcher.applyLookupPatch(&patch);
        if (patcher.getError() != KernelPatcher::Error::NoError) {
            SYSLOG("wred", "failed to apply agdp Piker-Alpha's patch %d", patcher.getError());
            patcher.clearError();
        }
    }

    if (graphicsDisplayPolicyMod & AGDP_CFGMAP) {
        // Does not function in 10.13.x, as the symbols have been stripped.
        // Abort on usage on 10.14 or newer.
        if (getKernelVersion() >= KernelVersion::Mojave)
            PANIC("wred", "adgpmod=cfgmap has no effect on 10.13.4, use agdpmod=ignore");
        KernelPatcher::RouteRequest request("__ZN25AppleGraphicsDevicePolicy5startEP9IOService",
            wrapGraphicsPolicyStart, orgGraphicsPolicyStart);
        patcher.routeMultiple(kextAGDPolicy.loadIndex, &request, 1, address, size);
    }
}

bool WRed::isGraphicsPolicyModRequired(DeviceInfo *info) {
    DBGLOG("wred", "detecting policy");
    // Graphics policy patches are only applicable to discrete GPUs.
    if (info->videoExternal.size() == 0) {
        DBGLOG("wred", "no external gpus");
        return false;
    }

    // Graphics policy patches do harm on Apple MacBooks, see:
    // https://github.com/acidanthera/bugtracker/issues/260
    if (info->firmwareVendor == DeviceInfo::FirmwareVendor::Apple) {
        DBGLOG("wred", "apple firmware");
        return false;
    }

    // We do not need AGDC patches on compatible devices.
    auto boardId = BaseDeviceInfo::get().boardIdentifier;
    DBGLOG("wred", "board is %s", boardId);
    const char *compatibleBoards[] {
        "Mac-00BE6ED71E35EB86",    // iMac13,1
        "Mac-27ADBB7B4CEE8E61",    // iMac14,2
        "Mac-4B7AC7E43945597E",    // MacBookPro9,1
        "Mac-77EB7D7DAF985301",    // iMac14,3
        "Mac-C3EC7CD22292981F",    // MacBookPro10,1
        "Mac-C9CF552659EA9913",    // ???
        "Mac-F221BEC8",            // MacPro5,1 (and MacPro4,1)
        "Mac-F221DCC8",            // iMac10,1
        "Mac-F42C88C8",            // MacPro3,1
        "Mac-FC02E91DDD3FA6A4",    // iMac13,2
        "Mac-2BD1B31983FE1663"     // MacBookPro11,3
    };
    for (size_t i = 0; i < arrsize(compatibleBoards); i++) {
        if (!strcmp(compatibleBoards[i], boardId)) {
            DBGLOG("wred", "disabling nvidia patches on model %s", boardId);
            return false;
        }
    }

    return true;
}

void WRed::wrapFramebufferInit(IOFramebuffer *fb) {
    bool backCopy = callbackWRED->gotConsoleVinfo && callbackWRED->resetFramebuffer == FB_COPY;
    bool zeroFill = callbackWRED->gotConsoleVinfo && callbackWRED->resetFramebuffer == FB_ZEROFILL;
    auto &info = callbackWRED->consoleVinfo;

    // Copy back usually happens in a separate call to frameBufferInit
    // Furthermore, v_baseaddr may not be available on subsequent calls, so we
    // have to copy
    if (backCopy && info.v_baseaddr) {
        // Note, this buffer is left allocated and never freed, yet there
        // actually is no way to free it.
        callbackWRED->consoleBuffer = Buffer::create<uint8_t>(info.v_rowbytes * info.v_height);
        if (callbackWRED->consoleBuffer)
            lilu_os_memcpy(callbackWRED->consoleBuffer, reinterpret_cast<uint8_t *>(info.v_baseaddr),
                info.v_rowbytes * info.v_height);
        else
            SYSLOG("wred", "console buffer allocation failure");
        // Even if we may succeed next time, it will be unreasonably dangerous
        info.v_baseaddr = 0;
    }

    uint8_t verboseBoot = *callbackWRED->gIOFBVerboseBootPtr;
    // For back copy we need a console buffer and no verbose
    backCopy = backCopy && callbackWRED->consoleBuffer && !verboseBoot;

    // Now check if the resolution and parameters match
    if (backCopy || zeroFill) {
        IODisplayModeID mode;
        IOIndex depth;
        IOPixelInformation pixelInfo;

        if (fb->getCurrentDisplayMode(&mode, &depth) == kIOReturnSuccess &&
            fb->getPixelInformation(mode, depth, kIOFBSystemAperture, &pixelInfo) == kIOReturnSuccess) {
            DBGLOG("wred", "fb info 1: %d:%d %u:%u:%u", mode, depth, pixelInfo.bytesPerRow, pixelInfo.bytesPerPlane,
                pixelInfo.bitsPerPixel);
            DBGLOG("wred", "fb info 2: %u:%u %s %u:%u:%u", pixelInfo.componentCount, pixelInfo.bitsPerComponent,
                pixelInfo.pixelFormat, pixelInfo.flags, pixelInfo.activeWidth, pixelInfo.activeHeight);

            if (info.v_rowbytes != pixelInfo.bytesPerRow || info.v_width != pixelInfo.activeWidth ||
                info.v_height != pixelInfo.activeHeight || info.v_depth != pixelInfo.bitsPerPixel) {
                backCopy = zeroFill = false;
                DBGLOG("wred", "this display has different mode");
            }
        } else {
            DBGLOG("wred", "failed to obtain display mode");
            backCopy = zeroFill = false;
        }
    }

    // For whatever reason not resetting Intel framebuffer (back copy mode)
    // twice works better.
    if (!backCopy) *callbackWRED->gIOFBVerboseBootPtr = 1;
    FunctionCast(wrapFramebufferInit, callbackWRED->orgFramebufferInit)(fb);
    if (!backCopy) *callbackWRED->gIOFBVerboseBootPtr = verboseBoot;

    // Finish the framebuffer initialisation by filling with black or copying
    // the image back.
    if (FramebufferViewer::getVramMap(fb)) {
        auto src = reinterpret_cast<uint8_t *>(callbackWRED->consoleBuffer);
        auto dst = reinterpret_cast<uint8_t *>(FramebufferViewer::getVramMap(fb)->getVirtualAddress());
        if (backCopy) {
            DBGLOG("wred", "attempting to copy...");
            // Here you can actually draw at your will, but looks like only on
            // Intel. On AMD you technically can draw too, but it happens for a
            // very short while, and is not worth it.
            lilu_os_memcpy(dst, src, info.v_rowbytes * info.v_height);
        } else if (zeroFill) {
            // On AMD we do a zero-fill to ensure no visual glitches.
            DBGLOG("wred", "doing zero-fill...");
            memset(dst, 0, info.v_rowbytes * info.v_height);
        }
    }
}

bool WRed::wrapGraphicsPolicyStart(IOService *that, IOService *provider) {
    auto boardIdentifier = BaseDeviceInfo::get().boardIdentifier;

    DBGLOG("wred", "agdp fix got board-id %s", boardIdentifier);
    auto oldConfigMap = OSDynamicCast(OSDictionary, that->getProperty("ConfigMap"));
    if (oldConfigMap) {
        auto rawConfigMap = oldConfigMap->copyCollection();
        if (rawConfigMap) {
            auto newConfigMap = OSDynamicCast(OSDictionary, rawConfigMap);
            if (newConfigMap) {
                auto none = OSString::withCString("none");
                if (none) {
                    newConfigMap->setObject(boardIdentifier, none);
                    none->release();
                    that->setProperty("ConfigMap", newConfigMap);
                }
            } else {
                SYSLOG("wred", "agdp fix failed to clone ConfigMap");
            }
            rawConfigMap->release();
        }
    } else {
        SYSLOG("wred", "agdp fix failed to obtain valid ConfigMap");
    }

    bool result = FunctionCast(wrapGraphicsPolicyStart, callbackWRED->orgGraphicsPolicyStart)(that, provider);
    DBGLOG("wred", "agdp start returned %d", result);

    return result;
}