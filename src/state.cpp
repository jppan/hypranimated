#include "hypranimated.hpp"

namespace hypranimated {

HANDLE PHANDLE = nullptr;
CFunctionHook* g_pRenderSnapshotHook = nullptr;
CFunctionHook* g_pStartWindowAnimationHook = nullptr;
CFunctionHook* g_pStartWorkspaceAnimationHook = nullptr;
bool g_unloading = false;

SConfig g_config;
std::vector<Hyprutils::Signal::CHyprSignalListener> g_listeners;
std::unordered_map<uintptr_t, SClosingAnimation> g_closing;
std::vector<SQueuedClosingRender> g_queuedClosingRenders;
std::unordered_map<MONITORID, UP<SMonitorShaderState>> g_monitorShaderStates;
std::unordered_map<MONITORID, PHLWORKSPACEREF> g_pendingWorkspaceSwitchFrom;
std::vector<SP<SWorkspaceSwitchRenderState>> g_workspaceSwitches;
std::unordered_set<IWindowTransformer*> g_animatedTransformers;
std::unordered_map<std::string, SAnimationConfigBackup> g_animationBackups;
bool g_reloadShaders = false;
std::optional<SEffectConfig> g_effectConfig;
std::string g_effectConfigKey;

} // namespace hypranimated
