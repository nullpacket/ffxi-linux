// SPDX-License-Identifier: GPL-3.0-or-later
// A small Ashita plugin written against the Ashita SDK, built natively like any other (cmake/ashita_plugins.cmake) for
// tests/test_ashita_plugins.cpp: it answers through the IAshitaCore the client hands it.
//   /xitest NAME        -> writes "xitest: party0=<our name> item1=<item 4096's name> spell1=<spell 1's name>" and
//                          queues "/echo from xitest" in mode 1
//   /raw REST           -> rewrites the line to REST in place (as Shorthand does) and lets it through
//   S2C 0x051           -> sets byte 4 of the modified packet to 0x42
//   C2S 0x0B6           -> blocked
#include "Ashita.h"

#include <cstring>
#include <string>

class XiTest final : public IPlugin
{
public:
    const char* GetName() const override
    {
        return "XiTest";
    }
    const char* GetAuthor() const override
    {
        return "ffxi-linux";
    }
    const char* GetDescription() const override
    {
        return "test plugin";
    }
    const char* GetLink() const override
    {
        return "";
    }
    double GetVersion() const override
    {
        return 1.0;
    }
    uint32_t GetFlags() const override
    {
        return static_cast<uint32_t>(Ashita::PluginFlags::Legacy);
    }
    bool Initialize(IAshitaCore* core, ILogManager* log, uint32_t id) override
    {
        core_ = core;
        log->Logf(4, "XiTest", "initialized as %u", id);
        return core->GetPluginManager()->IsLoaded("addons");
    }
    bool HandleCommand(int32_t mode, const char* command, bool injected) override
    {
        if (std::strncmp(command, "/raw ", 5) == 0)
        {
            const std::size_t n = std::strlen(command) - 5;
            std::memmove(const_cast<char*>(command), command + 5, n + 1);
            return false;
        }
        if (std::strncmp(command, "/xitest", 7) != 0)
        {
            return false;
        }
        auto*       res   = core_->GetResourceManager();
        const auto* item  = res->GetItemById(4096);
        const auto* spell = res->GetSpellById(1);
        core_->GetChatManager()->Writef(1, false, "xitest: party0=%s item1=%s spell1=%s mode=%d injected=%d",
                                        core_->GetMemoryManager()->GetParty()->GetMemberName(0),
                                        item ? item->Name[0] : "-", spell ? spell->Name[0] : "-", mode,
                                        injected ? 1 : 0);
        core_->GetChatManager()->QueueCommand(1, "/echo from xitest");
        return true;
    }
    bool HandleIncomingPacket(uint16_t id, uint32_t size, const uint8_t*, uint8_t* modified, uint32_t, const uint8_t*,
                              bool, bool) override
    {
        if (id == 0x051 && size > 4)
        {
            modified[4] = 0x42;
        }
        return false;
    }
    bool HandleOutgoingPacket(uint16_t id, uint32_t, const uint8_t*, uint8_t*, uint32_t, const uint8_t*, bool,
                              bool) override
    {
        return id == 0x0B6;
    }

private:
    IAshitaCore* core_ = nullptr;
};

__declspec(dllexport) IPlugin* __stdcall expCreatePlugin(const char*)
{
    return new XiTest();
}
__declspec(dllexport) void __stdcall expDestroyPlugin(void* instance)
{
    delete static_cast<XiTest*>(instance);
}
__declspec(dllexport) double __stdcall expGetInterfaceVersion(void)
{
    return ASHITA_INTERFACE_VERSION;
}
