# GLK PPS implementation boundaries

The implementation encodes the actual two GLK PCH PPS register banks and sequencing from pinned i915. It computes hardware/VBT delays, waits T1/T3, T8/T9 and T11/T12, orders AUX/VDD/panel/backlight requests and retains unobserved effects. The production body is exercised by a hosted register model; that model is not hardware proof.

The real display owner must supply its sleepable lock, native registry identity and PM/reset epoch validation, display-core/AUX power references, powered MMIO transport and firmware display handover. None is supplied by a true boolean from userspace. Missing callbacks or unknown panel choice refuse access. A revoked owner permanently freezes the object and its outstanding references; hardware recovery/retirement must establish when they may be released. No destructor frees an uncertain reference.

The actual 15IGL7 VBT/panel profile has not been captured. For BDB before191 a validated observed PWM entry establishes the upstream default PPS0; an unknown entry does not. Native eDP AUX/link training, panel binding, PWM registers, display pipes/planes, firmware takeover and framebuffer integration remain separate requirements. No MMIO is executed automatically, no driver was loaded and PPS0/black screen are not claimed fixed on hardware.
