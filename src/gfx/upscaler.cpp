// SPDX-License-Identifier: GPL-3.0-or-later
#include "gfx/upscaler.h"

namespace xi::gfx
{

UpscalerChoice chooseUpscaler(std::string_view upscaler, std::string_view dlssMode, std::string_view fsrMode,
                              bool dlssPossible, bool fsrPossible)
{
    UpscalerChoice         c;
    const std::string_view mode     = !fsrMode.empty()    ? fsrMode
                                      : !dlssMode.empty() ? dlssMode
                                                          : std::string_view("quality");
    bool                   wantDlss = false, wantFsr = false;
    if (upscaler == "off")
    {
        return c;
    }
    if (upscaler == "dlss")
    {
        wantDlss = true;
    }
    else if (upscaler == "fsr")
    {
        wantFsr = true;
    }
    else if (upscaler == "auto")
    {
        wantDlss = dlssPossible;
        wantFsr  = !dlssPossible;
    }
    else if (!dlssMode.empty())
    {
        wantDlss = true;
    }
    else if (!fsrMode.empty())
    {
        wantFsr = true;
    }
    if (wantDlss && dlssPossible)
    {
        c.kind = UpscalerKind::Dlss;
        c.mode = dlssMode.empty() ? std::string(mode) : std::string(dlssMode);
        return c;
    }
    if (wantDlss || wantFsr)
    {
        if (!fsrPossible)
        {
            c.note = wantDlss ? "DLSS needs an NVIDIA RTX GPU and a DLSS build, and FSR can't run here either"
                              : "FSR can't run on this GPU / build";
            return c;
        }
        c.kind = UpscalerKind::Fsr;
        c.mode = std::string(mode);
        if (wantDlss && upscaler != "auto")
        {
            c.note = "DLSS needs an NVIDIA RTX GPU and a DLSS build: using FSR instead";
        }
    }
    return c;
}

} // namespace xi::gfx
