//
//  kern_weg.hpp
//  WhateverRed
//
//  Copyright © 2017 vit9696. All rights reserved.
//  Copyright © 2022 ChefKiss Inc. All rights reserved.
//

#ifndef kern_weg_hpp
#define kern_weg_hpp

#include "kern_rad.hpp"
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_iokit.hpp>

class IOFramebuffer;
class IODisplay;

class WRed {
    public:
    void init();
    void deinit();

    private:
    /**
     *  Private self instance for callbacks
     */
    static WRed *callbackWRED;

    /**
     *  Radeon GPU fixes instance
     */
    RAD rad;

    /**
     *  FB_DETECT   autodetects based on the installed GPU.
     *  FB_RESET    enforces -v like usual patch.
     *  FB_COPY     enforces screen copy (default on IGPU).
     *  FB_ZEROFILL erases screen content (default on AMD).
     *  FB_NONE     does nothing.
     */
    enum FramebufferFixMode { FB_DETECT = 0, FB_RESET = 1, FB_COPY = 2, FB_ZEROFILL = 3, FB_NONE = 4, FB_TOTAL = 5 };

    /**
     *  Framebuffer distortion fix mode
     */
    uint32_t resetFramebuffer {FB_DETECT};

    /**
     *  Console info structure, taken from osfmk/console/video_console.h
     *  Last updated from XNU 4570.1.46.
     */
    struct vc_info {
        unsigned int v_height; /* pixels */
        unsigned int v_width;  /* pixels */
        unsigned int v_depth;
        unsigned int v_rowbytes;
        unsigned long v_baseaddr;
        unsigned int v_type;
        char v_name[32];
        uint64_t v_physaddr;
        unsigned int v_rows;         /* characters */
        unsigned int v_columns;      /* characters */
        unsigned int v_rowscanbytes; /* Actualy number of bytes used for display
                                        per row*/
        unsigned int v_scale;
        unsigned int v_rotate;
        unsigned int v_reserved[3];
    };

    /**
     *  Loaded vinfo
     */
    vc_info consoleVinfo {};

    /**
     *  Console buffer backcopy
     */
    uint8_t *consoleBuffer {nullptr};

    /**
     *  Original IOGraphics framebuffer init handler
     */
    mach_vm_address_t orgFramebufferInit {};

    /**
     *  Verbose boot global variable pointer
     */
    uint8_t *gIOFBVerboseBootPtr {nullptr};

    /**
     *  Original AppleGraphicsDevicePolicy start handler
     */
    mach_vm_address_t orgGraphicsPolicyStart {0};

    /**
     *  vinfo presence status
     */
    bool gotConsoleVinfo {false};

    /**
     *  Device identification spoofing for IGPU
     */
    bool hasIgpuSpoof {false};

    /**
     *  Device identification spoofing for GFX0
     */
    bool hasGfxSpoof {false};

    /**
     *  Maximum GFX naming index (due to ACPI name restrictions)
     */
    static constexpr uint8_t MaxExternalGfxIndex {9};

    /**
     *  GPU index used for GFXx naming in IORegistry
     *  Must be single digits (i.e. 0~9 inclusive).
     */
    uint8_t currentExternalGfxIndex {0};

    /**
     *  Maximum GFX slot naming index
     *  Should be 1~4 to display properly in NVIDIA panel.
     *  However, we permit more to match external GFX naming.
     */
    static constexpr uint8_t MaxExternalSlotIndex {10};

    /**
     *  GPU index used for AAPL,slot-name naming in IORegistry
     *  Should be 1~4 to display properly in NVIDIA panel.
     */
    uint8_t currentExternalSlotIndex {1};

    /**
     *  AppleGraphicsDisplayPolicy modifications if applicable.
     *
     *  AGDP_NONE     no modifications
     *  AGDP_DETECT   detect on firmware vendor and hardware installed
     *  AGDP_VIT9696  null config string size at strcmp
     *  AGDP_PIKERA   board-id -> board-ix replace
     *  AGDP_CFGMAP   add board-id with none to ConfigMap
     *  SET bit is used to distinguish from agpmod=detect.
     */
    enum GraphicsDisplayPolicyMod {
        AGDP_SET = 0x8000,
        AGDP_NONE_SET = AGDP_SET | 0,
        AGDP_DETECT = 1,
        AGDP_DETECT_SET = AGDP_SET | AGDP_DETECT,
        AGDP_VIT9696 = 2,
        AGDP_PIKERA = 4,
        AGDP_CFGMAP = 8,
        AGDP_PATCHES = AGDP_VIT9696 | AGDP_PIKERA | AGDP_CFGMAP
    };

    /**
     *  Current AppleGraphicsDisplayPolicy modifications.
     */
    int graphicsDisplayPolicyMod {AGDP_DETECT};

    /**
     *  Apply pre-kext patches and setup the configuration
     *
     *  @param patcher KernelPatcher instance
     */
    void processKernel(KernelPatcher &patcher);

    /**
     *  Patch kext if needed and prepare other patches
     *
     *  @param patcher KernelPatcher instance
     *  @param index   kinfo handle
     *  @param address kinfo load address
     *  @param size    kinfo memory size
     */
    void processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size);

    /**
     *  Parse AppleGraphicsDevicePolicy (AGDP) patch configuration
     *
     *  @param patcher KernelPatcher instance
     *  @param address agdp load address
     *  @param size    agdp memory size
     */
    void processGraphicsPolicyStr(const char *agdp);

    /**
     *  Apply AppleGraphicsDevicePolicy (AGDP) patches if any
     *
     *  @param patcher KernelPatcher instance
     *  @param address agdp load address
     *  @param size    agdp memory size
     */
    void processGraphicsPolicyMods(KernelPatcher &patcher, mach_vm_address_t address, size_t size);

    /**
     *  Check whether the graphics policy modification patches are required
     *
     *  @param info  device information
     *
     *  @return true if we should continue
     */
    bool isGraphicsPolicyModRequired(DeviceInfo *info);

    /**
     *  IOFramebuffer initialisation wrapper used for screen distortion fixes
     *
     *  @param fb  framebuffer instance
     */
    static void wrapFramebufferInit(IOFramebuffer *fb);

    /**
     *  AppleGraphicsDevicePolicy start wrapper used for black screen fixes in
     * AGDP_CFGMAP mode
     *
     *  @param that      agdp instance
     *  @param provider  agdp provider
     *
     *  @return agdp start status
     */
    static bool wrapGraphicsPolicyStart(IOService *that, IOService *provider);
};

#endif /* kern_weg_hpp */