extern "C" {
#include <qemu-common.h>
#include <cpu-all.h>
#include <exec-all.h>
}

#include "NdisHandlers.h"
#include "Ndis.h"
#include <s2e/S2E.h>
#include <s2e/ConfigFile.h>
#include <s2e/Utils.h>
#include <s2e/StateManager.h>
#include <s2e/Plugins/WindowsInterceptor/WindowsImage.h>
#include <klee/Solver.h>

#include <iostream>

using namespace s2e::windows;

namespace s2e {
namespace plugins {

S2E_DEFINE_PLUGIN(NdisHandlers, "Basic collection of NDIS API functions.", "NdisHandlers",
                  "FunctionMonitor", "WindowsMonitor", "ModuleExecutionDetector");

void NdisHandlers::initialize()
{

    ConfigFile *cfg = s2e()->getConfig();

    m_functionMonitor = static_cast<FunctionMonitor*>(s2e()->getPlugin("FunctionMonitor"));
    m_windowsMonitor = static_cast<WindowsMonitor*>(s2e()->getPlugin("WindowsMonitor"));
    m_detector = static_cast<ModuleExecutionDetector*>(s2e()->getPlugin("ModuleExecutionDetector"));

    ConfigFile::string_list mods = cfg->getStringList(getConfigKey() + ".moduleIds");
    if (mods.size() == 0) {
        s2e()->getWarningsStream() << "No modules to track configured for the NdisHandlers plugin" << std::endl;
        return;
    }


    foreach2(it, mods.begin(), mods.end()) {
        m_modules.insert(*it);
    }

    m_windowsMonitor->onModuleLoad.connect(
            sigc::mem_fun(*this,
                    &NdisHandlers::onModuleLoad)
            );


}


void NdisHandlers::onModuleLoad(
        S2EExecutionState* state,
        const ModuleDescriptor &module
        )
{
    const std::string *s = m_detector->getModuleId(module);
    if (!s || (m_modules.find(*s) == m_modules.end())) {
        //Not the right module we want to intercept
        return;
    }

    //We loaded the module, instrument the entry point
    if (!module.EntryPoint) {
        s2e()->getWarningsStream() << "NdisHandlers: Module has no entry point ";
        module.Print(s2e()->getWarningsStream());
    }

    FunctionMonitor::CallSignal* entryPoint;
    REGISTER_NDIS_ENTRY_POINT(entryPoint, module.ToRuntime(module.EntryPoint), entryPoint);

    Imports I;
    if (!m_windowsMonitor->getImports(state, module, I)) {
        s2e()->getWarningsStream() << "NdisHandlers: Could not read imports for module ";
        module.Print(s2e()->getWarningsStream());
        return;
    }

    //Register all the relevant imported functions
    Imports::iterator it = I.find("ndis.sys");
    if (it == I.end()) {
        s2e()->getWarningsStream() << "NdisHandlers: Could not read imports for ndis.sys for module";
        module.Print(s2e()->getWarningsStream());
        return;
    }

    ImportedFunctions &funcs = (*it).second;
    ImportedFunctions::iterator fit = funcs.find("NdisMRegisterMiniport");
    if (fit == funcs.end()) {
        s2e()->getWarningsStream() << "NdisHandlers: Could not find NdisMRegisterMiniport in ndis.sys";
        module.Print(s2e()->getWarningsStream());
        return;
    }
    REGISTER_NDIS_ENTRY_POINT(entryPoint, (*fit).second, NdisMRegisterMiniport);


}


////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::entryPoint(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling NDIS entry point "
                << " at " << hexval(state->getPc()) << std::endl;

    signal->connect(sigc::mem_fun(*this, &NdisHandlers::entryPointRet));

}

void NdisHandlers::entryPointRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from NDIS entry point "
                << " at " << hexval(state->getPc()) << std::endl;

    //Check the success status
    klee::ref<klee::Expr> eax = state->readCpuRegister(offsetof(CPUState, regs[R_EAX]), klee::Expr::Int32);

    //XXX: do it better
    klee::ref<klee::Expr> eq = klee::SgeExpr::create(eax, klee::ConstantExpr::create(0, eax.get()->getWidth()));
    bool isTrue;
    if (s2e()->getExecutor()->getSolver()->mustBeTrue(klee::Query(state->constraints, eq), isTrue)) {
        if (!isTrue) {
            s2e()->getMessagesStream(state) << "Killing state "  << state->getID() <<
                    " because EntryPoint failed with 0x" << std::hex << eax << std::endl;
            s2e()->getExecutor()->terminateStateOnExit(*state);
            return;
        }
    }

    StateManager *mgr = StateManager::getManager(s2e());
    mgr->succeededState(state);

    if (mgr->empty()) {
        mgr->killAllButOneSuccessful();
    }
}
////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////

void NdisHandlers::NdisMRegisterMiniport(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::NdisMRegisterMiniportRet));

    //Extract the function pointers from the passed data structure
    uint32_t pMiniport;
    if (!state->readMemoryConcrete(state->getSp() + sizeof(pMiniport) * (1+1), &pMiniport, sizeof(pMiniport))) {
        s2e()->getMessagesStream() << "Could not read pMiniport address from the stack" << std::endl;
        return;
    }

    s2e::windows::NDIS_MINIPORT_CHARACTERISTICS32 Miniport;
    if (!state->readMemoryConcrete(pMiniport, &Miniport, sizeof(Miniport))) {
        s2e()->getMessagesStream() << "Could not read NDIS_MINIPORT_CHARACTERISTICS" << std::endl;
        return;
    }

    //Register each handler
    FunctionMonitor::CallSignal* entryPoint;
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.CheckForHangHandler, CheckForHang);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.InitializeHandler, InitializeHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.DisableInterruptHandler, DisableInterruptHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.EnableInterruptHandler, EnableInterruptHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.HaltHandler, HaltHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.HandleInterruptHandler, HandleInterruptHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.ISRHandler, ISRHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.QueryInformationHandler, QueryInformationHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.ReconfigureHandler, ReconfigureHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.ResetHandler, ResetHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.SendPacketsHandler, SendPacketsHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.SetInformationHandler, SetInformationHandler);
    REGISTER_NDIS_ENTRY_POINT(entryPoint, Miniport.TransferDataHandler, TransferDataHandler);

}

void NdisHandlers::NdisMRegisterMiniportRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    s2e()->getExecutor()->jumpToSymbolicCpp(state);

    //Get the return value
    uint32_t eax;
    if (!state->readCpuRegisterConcrete(offsetof(CPUState, regs[R_EAX]), &eax, sizeof(eax))) {
        s2e()->getWarningsStream() << __FUNCTION__  << ": return status is not concrete" << std::endl;
        return;
    }

    //Replace the return value with a symbolic value
    if ((int)eax>=0) {
        klee::ref<klee::Expr> ret = state->createSymbolicValue(klee::Expr::Int32, __FUNCTION__);
        state->writeCpuRegister(offsetof(CPUState, regs[R_EAX]), ret);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::CheckForHang(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::NdisMRegisterMiniportRet));
}

void NdisHandlers::CheckForHangRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;

}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::InitializeHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::InitializeHandlerRet));
}

void NdisHandlers::InitializeHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::DisableInterruptHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::DisableInterruptHandlerRet));
}

void NdisHandlers::DisableInterruptHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::EnableInterruptHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::EnableInterruptHandlerRet));
}

void NdisHandlers::EnableInterruptHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::HaltHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::HaltHandlerRet));
}

void NdisHandlers::HaltHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::HandleInterruptHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::HandleInterruptHandlerRet));
}

void NdisHandlers::HandleInterruptHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::ISRHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::ISRHandlerRet));
}

void NdisHandlers::ISRHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::QueryInformationHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::QueryInformationHandlerRet));
}

void NdisHandlers::QueryInformationHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::ReconfigureHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::ReconfigureHandlerRet));
}

void NdisHandlers::ReconfigureHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::ResetHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::ResetHandlerRet));
}

void NdisHandlers::ResetHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::SendPacketsHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::SendPacketsHandlerRet));
}

void NdisHandlers::SendPacketsHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::SetInformationHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::SetInformationHandlerRet));
}

void NdisHandlers::SetInformationHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////
void NdisHandlers::TransferDataHandler(S2EExecutionState* state, FunctionMonitor::ReturnSignal *signal)
{
    s2e()->getDebugStream(state) << "Calling " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
    signal->connect(sigc::mem_fun(*this, &NdisHandlers::TransferDataHandlerRet));
}

void NdisHandlers::TransferDataHandlerRet(S2EExecutionState* state)
{
    s2e()->getDebugStream(state) << "Returning from " << __FUNCTION__ << " at " << hexval(state->getPc()) << std::endl;
}

} // namespace plugins
} // namespace s2e