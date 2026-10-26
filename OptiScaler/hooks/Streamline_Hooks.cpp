#include <pch.h>

#include "Streamline_Hooks.h"
#include "LibraryLoad_Hooks.h"

#include <Util.h>
#include <intrin.h>
#include <Config.h>
#include <dlssnr/DlssNr_StreamlinePicture.h>

#include <nvapi/fakenvapi.h>
#include <misc/IdentifyGpu.h>
#include <hooks/Reflex_Hooks.h>
#include <menu/menu_overlay_base.h>
#include <framegen/nvngx/Nvngx_FG.h>
#include <framegen/dlssg/DlssgTelemetry.h>
#include <proxies/KernelBase_Proxy.h>
#include <proxies/Streamline_Proxy.h>
#include <imgui/ImGuiNotify.hpp>

#include <json.hpp>
#include <sl1_reflex.h>
#include <magic_enum.hpp>
#include "detours/detours.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace
{
thread_local const sl::Preferences* gamePluginPreferences = nullptr;

// Detours supports only one in-flight transaction per process at a time (its own docs: only one
// thread may perform DetourTransactionBegin/Attach/Detach/Commit at once). hookDlss/hookDlssg/
// hookInterposer/etc. each run their own complete Begin...Commit sequence synchronously from
// inside OptiScaler's own LoadLibraryW hook, exactly when Streamline's plugin manager loads each
// sl.*.dll -- with nothing serializing them, two of these racing (cross-thread, or just close
// together in time) corrupts DetourTransactionCommit's state and fails it with
// ERROR_INVALID_OPERATION (0x10DD), which Streamline's own exception handler then treats as fatal.
// This mutex serializes every Begin...Commit sequence in this file against every other one.
std::mutex g_detoursTransactionMutex;

// The original a hook calls is read on game threads while a plugin may be hooked on another
template <typename T> static T LoadOriginal(T& original)
{
    return std::atomic_ref<T>(original).load(std::memory_order_acquire);
}

template <typename T> static void StoreOriginal(T& original, T value)
{
    std::atomic_ref<T>(original).store(value, std::memory_order_release);
}

struct GamePluginLoadScope
{
    const sl::Preferences* previous;
    explicit GamePluginLoadScope(const sl::Preferences& preferences)
        : previous(std::exchange(gamePluginPreferences, &preferences))
    {
    }
    ~GamePluginLoadScope() { gamePluginPreferences = previous; }
};
} // namespace

HMODULE StreamlineHooks::LoadIsolatedGamePlugin(LPCWSTR requestedPath)
{
    if (!gamePluginPreferences)
        return nullptr;
    const auto& state = State::Instance();
    if (state.activeFgOutput != FGOutput::DLSSG || !StreamlineProxy::IsD3D12Inited())
        return nullptr;

    const auto shared = GetModuleHandleW(requestedPath);
    if (!shared)
        return nullptr;
    const wchar_t* name = shared == state.optiSlCommon   ? L"sl.common.dll"
                          : shared == state.optiSlReflex ? L"sl.reflex.dll"
                          : shared == state.optiSlPCL    ? L"sl.pcl.dll"
                                                         : nullptr;
    // sl.common has no public feature function from which to resolve its active OTA module.
    auto path = std::filesystem::path(requestedPath).lexically_normal().wstring();
    to_lower_in_place(path);
    if (!name && path.find(L"\\sl_common_") != std::wstring::npos && path.find(L"\\versions\\") != std::wstring::npos)
        name = L"sl.common.dll";
    if (!name)
        return nullptr;

    // Two interposers cannot own the same plugin's globals. Prefer the game's own copy when
    // NVIDIA's override selection points it at an already active OptiScaler plugin.
    for (uint32_t i = 0; i < gamePluginPreferences->numPathsToPlugins; ++i)
    {
        const auto directory = gamePluginPreferences->pathsToPlugins[i];
        if (!directory)
            continue;
        const auto candidate = std::filesystem::path(directory) / name;
        const auto privateDirectory = std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"streamline";
        if (Util::IsSubpath(candidate, privateDirectory) || GetModuleHandleW(candidate.c_str()) == shared ||
            !std::filesystem::exists(candidate))
            continue;
        LOG_INFO("Keeping game Streamline plugin separate from DLSSG output: {}",
                 wstring_to_string(candidate.wstring()));
        return LibraryLoadHooks::LoadLibraryCheckW(candidate.wstring(), candidate.c_str());
    }
    return nullptr;
}

// A null frame token from the game (Indiana Jones sends one to slPCLSetMarker at start). Reading it crashes, so the
// hooks that read the token pass the call straight on. Logged once per hook
static bool NullFrameToken(const sl::FrameToken& frame, const char* hook, std::atomic_bool& logged)
{
    // Through a volatile pointer: a reference is never null to the compiler, which would drop a plain check
    const void* volatile framePtr = &frame;

    if (framePtr != nullptr)
        return false;

    if (!logged.exchange(true))
        LOG_WARN("{}: null frame token from the game, passed on unread", hook);

    return true;
}

static bool IsSL1AndFGActive()
{
    const auto& state = State::Instance();

    return state.streamlineVersion.major == 1 && state.activeFgInput == FGInput::DLSSG &&
           state.activeFgOutput != FGOutput::NoFG;
}

// Game's SL1 DLSSG has to stay inert when we use its inputs or provide DLSSG ourselves,
// otherwise its swapchain hooks end up stacked on top of our FG swapchain
static bool IsSL1AndDLSSGManaged()
{
    const auto& state = State::Instance();

    return state.streamlineVersion.major == 1 &&
           (state.activeFgInput == FGInput::DLSSG || state.activeFgOutput == FGOutput::DLSSG);
}

static void PatchSL1PluginJson(nlohmann::json& configJson)
{
    if (!IsSL1AndDLSSGManaged())
        return;

    // Once per plugin load: says in a user's log why the game's own SL1 frame generation does nothing
    LOG_INFO("SL1 DLSSG kept inert, OptiScaler manages it (FG Input {}, FG Output {}): plugin hooks and tags cleared",
             magic_enum::enum_name(State::Instance().activeFgInput),
             magic_enum::enum_name(State::Instance().activeFgOutput));

    if (configJson.contains("/hooks"_json_pointer))
        configJson["hooks"].clear();

    if (configJson.contains("/exclusive_hooks"_json_pointer))
        configJson["exclusive_hooks"].clear();

    if (configJson.contains("/external/feature/tags"_json_pointer))
        configJson["external"]["feature"]["tags"].clear();

    if (configJson.contains("/vsync/supported"_json_pointer))
        configJson["vsync"]["supported"] = true;

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false;
}

char* StreamlineHooks::trimStreamlineLog(const char* msg)
{
    char* result = (char*) malloc(strlen(msg) + 1);
    if (!result)
        return nullptr;

    strcpy(result, msg);

    size_t length = strlen(result);
    if (length > 0 && result[length - 1] == '\n')
    {
        result[length - 1] = '\0';
    }

    return result;
}

void StreamlineHooks::streamlineLogCallback(sl::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);
    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl::LogType::eWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl::LogType::eInfo:
            LOG_INFO("{}", trimmed_msg);
            break;
        case sl::LogType::eError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl::LogType::eCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback != nullptr)
        o_logCallback(type, msg);
}

sl::Result StreamlineHooks::hkslInit(const sl::Preferences& pref, uint64_t sdkVersion)
{
    LOG_FUNC();

    // The game's own Streamline, not one OptiScaler started for its DLSS frame generation (ReflexHooks::isGameCall)
    if (Util::GetCallerModule(_ReturnAddress()) != Util::GetCallerModule((void*) &StreamlineHooks::hkslInit))
        State::Instance().gameCalledSlInit = true;

    sl::Preferences localPref = pref;
    GamePluginLoadScope pluginScope(localPref);

    if (localPref.logMessageCallback != &streamlineLogCallback)
        o_logCallback = localPref.logMessageCallback;
    localPref.logLevel = sl::LogLevel::eCount;
    localPref.logMessageCallback = &streamlineLogCallback;

    // renderAPI is optional so need to be careful, should only matter for Vulkan
    renderApi = localPref.renderAPI;

    State::Instance().slFGInputs.reportEngineType(localPref.engine);

    // Treat engine type set in Streamline as ground truth
    if (localPref.engine == sl::EngineType::eUnreal)
        State::Instance().gameQuirks |= GameQuirk::ForceUnrealEngine;

    std::filesystem::path localSlPath(Config::Instance()->MainDllPath.value());
    localSlPath = localSlPath / L"streamline"; // Hardcoded streamline folder

    auto localSlPathStr = localSlPath.wstring();
    std::wstring minimalSlPathStr;

    std::vector<const wchar_t*> storage(localPref.pathsToPlugins, localPref.pathsToPlugins + localPref.numPathsToPlugins);

    const bool isNvngxFG = State::Instance().activeFgInput == FGInput::NvngxFG;
    const bool ignoreStreamlineOTA = Config::Instance()->FGStreamlineIgnoreOTA.value_or_default();
    bool localPathInserted = false;

    auto insertLocalPathOnce = [&](const wchar_t* pathToInsert)
    {
        if (localPathInserted)
            return;

        storage.insert(storage.begin(), pathToInsert);
        localPref.pathsToPlugins = storage.data();
        localPref.numPathsToPlugins = (uint32_t) storage.size();
        localPathInserted = true;
    };

    // Plugins confirmed (via Starfield minidump analysis) to load cleanly from a caller whose
    // declared SDK version is far older than ours. sl.dlss, sl.dlss_d, sl.deepdvc and sl.directsr
    // each access-violate (null read at a small offset) when loaded this way instead -- a bug
    // inside NVIDIA's own closed-source plugin, not something tied to one specific file's JSON.
    static constexpr const wchar_t* kSafeStreamlinePlugins[] = { L"sl.common.dll", L"sl.dlss.dll", L"sl.dlss_g.dll",
                                                                  L"sl.reflex.dll", L"sl.pcl.dll" };

    // Builds (or refreshes) a trimmed copy of the streamline folder containing only the plugins
    // above, for handing to a caller we don't trust with the full set. Rebuilt every call rather
    // than cached, since it's a handful of small files and this only runs for the rare mismatched-
    // SDK case.
    auto ensureMinimalPluginFolder = [&]() -> std::wstring
    {
        std::filesystem::path minimalPath = localSlPath / L"minimal";
        std::error_code ec;
        std::filesystem::create_directories(minimalPath, ec);

        for (const wchar_t* name : kSafeStreamlinePlugins)
        {
            std::filesystem::path src = localSlPath / name;
            if (std::filesystem::exists(src, ec))
                std::filesystem::copy_file(src, minimalPath / name,
                                            std::filesystem::copy_options::overwrite_existing, ec);
        }

        return minimalPath.wstring();
    };

    // Replace the SL files to allow for MFG
    if (isNvngxFG && std::filesystem::exists(localSlPath / L"sl.common.dll"))
    {
        std::filesystem::path pluginsDir;

        // Find the first path that contains sl.common.dll
        // If storage is empty, look in the exe folder. pathsToPlugins is an optional field
        if (storage.empty())
        {
            std::filesystem::path exeFolder = Util::ExePath().parent_path();
            if (std::filesystem::exists(exeFolder / L"sl.common.dll"))
            {
                pluginsDir = exeFolder;
            }
        }
        else
        {
            for (const wchar_t* pathStr : storage)
            {
                if (!pathStr)
                    continue;

                std::filesystem::path p = pathStr;
                if (std::filesystem::exists(p / L"sl.common.dll"))
                {
                    pluginsDir = p;
                    break;
                }
            }
        }

        std::vector<std::string> missingDlls;
        bool hasNewerPlugin = false;

        if (!pluginsDir.empty() && std::filesystem::exists(pluginsDir))
        {
            for (const auto& entry : std::filesystem::directory_iterator(pluginsDir))
            {
                if (!entry.is_regular_file())
                    continue;

                std::wstring filename = entry.path().filename().wstring();

                std::wstring lowerName = filename;
                to_lower_in_place(lowerName);

                // Skip interposer
                if (lowerName == L"sl.interposer.dll")
                    continue;

                const bool isSlDll = lowerName.starts_with(L"sl.") && lowerName.ends_with(L".dll");
                const bool isNvLowLatency = lowerName == L"nvlowlatencyvk.dll";

                if (isSlDll || isNvLowLatency)
                {
                    std::filesystem::path localDllPath = localSlPath / filename;

                    // Check if localSlPath also has this DLL
                    if (!std::filesystem::exists(localDllPath))
                    {
                        missingDlls.push_back(entry.path().filename().string());
                    }
                    else
                    {
                        // Compare versions
                        version_t pluginVer, pluginProdVer;
                        version_t localVer, localProdVer;

                        bool gotPluginVer = Util::GetFileVersion(entry.path().wstring(), &pluginVer, &pluginProdVer);
                        bool gotLocalVer = Util::GetFileVersion(localDllPath.wstring(), &localVer, &localProdVer);

                        if (gotPluginVer && gotLocalVer)
                        {
                            if (localVer > pluginVer)
                            {
                                hasNewerPlugin = true;
                            }
                        }
                    }
                }
            }
        }

        // Insert local path only if a newer plugin was found
        if (hasNewerPlugin)
        {
            LOG_DEBUG("Making the game use local streamline files");
            insertLocalPathOnce(localSlPathStr.c_str());

            if (!missingDlls.empty())
            {
                std::string toastMsg = "You are missing the following dlls from the streamline folder:\n";
                for (const auto& missingDll : missingDlls)
                {
                    toastMsg += "- " + missingDll + "\n";
                }

                ImGui::InsertNotification({ ImGuiToastType::Warning, 20000, toastMsg.c_str() });
            }
        }
    }

    // Keep Streamline off the driver's OTA cache entirely -- but only if OptiScaler/streamline
    // actually has a plugin set to fall back on. A caller whose declared SDK version is far older
    // than ours (Starfield: v2.2 vs our v2.11) crashes on four of our plugins specifically
    // (see kSafeStreamlinePlugins above) regardless of which interposer binary executes the call
    // or what SDK version we claim -- confirmed via minidump analysis, not a guess. For that case,
    // hand it the trimmed, known-safe subset instead of either risking the crash or giving up
    // on blocking OTA entirely.
    const uint32_t callerSdkMajor = static_cast<uint32_t>(sdkVersion >> 48);
    const uint32_t callerSdkMinor = static_cast<uint32_t>((sdkVersion >> 32) & 0xFFFF);
    const bool sdkMatchesLocal = callerSdkMajor == SL_VERSION_MAJOR && callerSdkMinor == SL_VERSION_MINOR;

    if (ignoreStreamlineOTA && std::filesystem::exists(localSlPath / L"sl.common.dll"))
    {
        if (sdkMatchesLocal)
        {
            LOG_DEBUG("Making the game use local streamline files (ignoring OTA)");
            insertLocalPathOnce(localSlPathStr.c_str());
        }
        else
        {
            minimalSlPathStr = ensureMinimalPluginFolder();
            LOG_WARN("StreamlineIgnoreOTA: caller declares SDK v{}.{} (OptiScaler/streamline built "
                     "for v{}.{}) -- using only the known-safe plugin subset (sl.common, sl.dlss, "
                     "sl.dlss_g, sl.reflex, sl.pcl) for this caller instead of the full folder",
                     callerSdkMajor, callerSdkMinor, SL_VERSION_MAJOR, SL_VERSION_MINOR);
            insertLocalPathOnce(minimalSlPathStr.c_str());
        }

        LOG_INFO("StreamlineIgnoreOTA: blocking Streamline's OTA/downloaded plugins, using OptiScaler/streamline only");
        localPref.flags &= ~sl::PreferenceFlags::eAllowOTA;
        localPref.flags &= ~sl::PreferenceFlags::eLoadDownloadedPlugins;
    }
    else if (ignoreStreamlineOTA)
    {
        LOG_WARN("StreamlineIgnoreOTA is enabled, but OptiScaler/streamline/sl.common.dll is missing -- "
                 "leaving Streamline's own OTA/downloaded-plugin resolution in place");
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgOutput == FGOutput::DLSSG)
    {
        std::vector<sl::Feature> localFeaturesToLoad(pref.featuresToLoad, pref.featuresToLoad + pref.numFeaturesToLoad);
        std::erase(localFeaturesToLoad, sl::kFeatureDLSS_G);

        localPref.featuresToLoad = localFeaturesToLoad.data();
        localPref.numFeaturesToLoad = localFeaturesToLoad.size();

        // return so that localFeaturesToLoad is valid
        return o_slInit(localPref, sdkVersion);
    }

    return o_slInit(localPref, sdkVersion);
}

sl::Result StreamlineHooks::hkslIsFeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapterInfo)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    return o_slIsFeatureSupported(feature, adapterInfo);
}

sl::Result StreamlineHooks::hkslIsFeatureLoaded(sl::Feature feature, bool& loaded)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        loaded = true;
        return sl::Result::eOk;
    }

    return o_slIsFeatureLoaded(feature, loaded);
}

sl::Result StreamlineHooks::hkslGetFeatureRequirements(sl::Feature feature, sl::FeatureRequirements& requirements)
{
    if (feature == sl::kFeatureDLSS_G)
        return sl::Result::eOk;

    return o_slGetFeatureRequirements(feature, requirements);
}

sl::Result StreamlineHooks::hkslGetFeatureVersion(sl::Feature feature, sl::FeatureVersion& version)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        version.versionSL = { State::Instance().streamlineVersion.major, State::Instance().streamlineVersion.minor,
                              State::Instance().streamlineVersion.patch };
        version.versionNGX = { 4, 2, 0 };

        return sl::Result::eOk;
    }

    return o_slGetFeatureVersion(feature, version);
}

// FG Input DLSSG: what OptiScaler's FG output tells the game. The max lets the game's menu offer what our output can
// do (3x/4x), not just 2x; presented frames are the real ones of our FG.
static void FillDlssgInputState(sl::DLSSGState& state)
{
    auto& optiState = State::Instance();
    auto fg = optiState.CurrentFG();

    const bool fgRunning = fg != nullptr && fg->IsActive() && !fg->IsPaused();
    state.numFramesActuallyPresented =
        FGFollowGame::Presented(fgRunning, fgRunning ? static_cast<int>(fg->GetInterpolatedFrameCount()) : 0);

    // Struct version 1 ends at 56 bytes, ahead of this field
    if (state.structVersion < 2)
        return;

    // Before our FG swapchain exists nothing is known yet. XeFG's max needs an XeFG context, and a throwaway one here
    // would race ours (Intel R5), so it is reported once our XeFG exists.
    const int reported = FGFollowGame::ReportedMax(fg != nullptr ? fg->GetMaxInterpolationCount() : 0);
    state.numFramesToGenerateMax = reported;

    // GetState runs every frame: log a change only
    static std::atomic<int> lastReported { 0 };
    if (lastReported.exchange(reported) != reported)
        LOG_INFO("Telling the game DLSS Frame Generation goes up to {}x ({} output)", reported + 1,
                 magic_enum::enum_name(optiState.activeFgOutput));
}

// FG Input DLSSG: record the game's own DLSS FG choice, which OptiScaler's FG follows while Enabled is auto.
// mode and numFramesToGenerate are struct version 1 fields, safe to read from any version.
static void RecordGameDlssgRequest(const sl::DLSSGOptions& options)
{
    auto& state = State::Instance();
    const auto next = FGFollowGame::FromGame(options.mode == sl::DLSSGMode::eOff, options.numFramesToGenerate);
    const auto previous = state.dlssgInputGameRequest.exchange(next);

    // While off, games keep resending their menu's multiplier (Witcher 3 alternates 2x/4x): nothing to report
    if (next == previous || (!next.enabled && !previous.enabled))
        return;

    const bool follows = Config::Instance()->FGFollowsGame();
    LOG_INFO("Game set DLSS Frame Generation {} {}x ({}): {}", next.enabled ? "on" : "off", next.count + 1,
             magic_enum::enum_name(options.mode),
             follows ? "OptiScaler FG follows it"
                     : "not followed, Frame Generation is set in OptiScaler (or Streamline 1)");

    if (FGFollowGame::NeedsFGChange(follows, previous, next, state.activeFgOutput == FGOutput::XeFG))
        state.fgChanged = true;
}

static sl::Result dummy_slDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                        const sl::DLSSGOptions* options)
{
    FillDlssgInputState(state);

    if (state.structVersion >= 2)
        state.bIsVsyncSupportAvailable = sl::Boolean::eTrue;

    state.estimatedVRAMUsageInBytes = 300 * 1024 * 1024;

    return sl::Result::eOk;
}

static sl::Result dummy_slDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    RecordGameDlssgRequest(options);
    return sl::Result::eOk;
}

sl::Result StreamlineHooks::hkslGetFeatureFunction(sl::Feature feature, const char* functionName, void*& function)
{
    if (feature == sl::kFeatureDLSS_G)
    {
        if (strcmp(functionName, "slDLSSGSetOptions") == 0)
        {
            function = &dummy_slDLSSGSetOptions;

            return sl::Result::eOk;
        }

        if (strcmp(functionName, "slDLSSGGetState") == 0)
        {
            function = &dummy_slDLSSGGetState;

            return sl::Result::eOk;
        }
    }

    return o_slGetFeatureFunction(feature, functionName, function);
}

sl::Result StreamlineHooks::hkslSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                                       uint32_t numTags, sl::CommandBuffer* cmdBuffer)
{
    if (renderApi == sl::RenderAPI::eD3D11)
    {
        LOG_ERROR("hkslSetTag doesn't support DX11");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    if (tags == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTag(viewport, tags, numTags, cmdBuffer);
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG &&
        State::Instance().gameQuirks[GameQuirk::IgnoreTagsWithoutHudlessForFG])
    {
        bool hasDepth = false;
        bool hasMVs = false;
        bool hasHudless = false;

        for (uint32_t i = 0; i < numTags; i++)
        {
            if (tags[i].resource == nullptr || tags[i].resource->native == nullptr)
                continue;

            if (tags[i].type == sl::kBufferTypeDepth)
                hasDepth = true;

            if (tags[i].type == sl::kBufferTypeMotionVectors)
                hasMVs = true;

            if (tags[i].type == sl::kBufferTypeHUDLessColor)
                hasHudless = true;
        }

        // Try to skip a DLSS call
        if (hasDepth && hasMVs && !hasHudless)
        {
            LOG_DEBUG("Skipping the FG tagging of potential DLSS resources");
            return o_slSetTag(viewport, tags, numTags, cmdBuffer);
        }
    }

    for (uint32_t i = 0; i < numTags; i++)
    {
        const auto typeEnum = (BufferType) tags[i].type;

        if (tags[i].resource == nullptr || tags[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        // Cyberpunk hudless state fix for RDNA 2
        if (State::Instance().gameQuirks & GameQuirk::CyberpunkHudlessState && renderApi != sl::RenderAPI::eVulkan &&
            tags[i].resource->state ==
                (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
            tags[i].type == sl::kBufferTypeHUDLessColor)
        {
            tags[i].resource->state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            LOG_TRACE("Changing hudless resource state");
        }

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (tags[i].type == sl::kBufferTypeHUDLessColor || tags[i].type == sl::kBufferTypeDepth ||
             tags[i].type == sl::kBufferTypeHiResDepth || tags[i].type == sl::kBufferTypeLinearDepth ||
             tags[i].type == sl::kBufferTypeMotionVectors || tags[i].type == sl::kBufferTypeUIColorAndAlpha ||
             tags[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(tags[i], cmdBuffer, 0);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));
        }
    }

    auto result = o_slSetTag(viewport, tags, numTags, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslSetTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport,
                                               const sl::ResourceTag* resources, uint32_t numResources,
                                               sl::CommandBuffer* cmdBuffer)
{
    if (renderApi == sl::RenderAPI::eD3D11)
    {
        LOG_ERROR("hkslSetTagForFrame doesn't support DX11");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    static std::atomic_bool nullTokenLogged = false;
    if (NullFrameToken(frame, __FUNCTION__, nullTokenLogged))
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);

    if (renderApi == sl::RenderAPI::eCount)
        LOG_WARN("Incomplete Streamline hooks");

    if (resources == nullptr)
    {
        LOG_WARN("Game trying to remove a tag");
        return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    }

    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));

    if (State::Instance().activeFgInput == FGInput::DLSSG &&
        State::Instance().gameQuirks[GameQuirk::IgnoreTagsWithoutHudlessForFG])
    {
        bool hasDepth = false;
        bool hasMVs = false;
        bool hasHudless = false;

        for (uint32_t i = 0; i < numResources; i++)
        {
            if (resources[i].resource == nullptr || resources[i].resource->native == nullptr)
                continue;

            if (resources[i].type == sl::kBufferTypeDepth)
                hasDepth = true;

            if (resources[i].type == sl::kBufferTypeMotionVectors)
                hasMVs = true;

            if (resources[i].type == sl::kBufferTypeHUDLessColor)
                hasHudless = true;
        }

        // Try to skip a DLSS call
        if (hasDepth && hasMVs && !hasHudless)
        {
            LOG_DEBUG("Skipping the FG tagging of potential DLSS resources");
            return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
        }
    }

    for (uint32_t i = 0; i < numResources; i++)
    {
        const auto typeEnum = (BufferType) resources[i].type;

        if (resources[i].resource == nullptr || resources[i].resource->native == nullptr)
        {
            LOG_TRACE("Resource of type: {} is null, continuing", magic_enum::enum_name(typeEnum));
            continue;
        }

        if (State::Instance().activeFgInput == FGInput::DLSSG &&
            (resources[i].type == sl::kBufferTypeHUDLessColor || resources[i].type == sl::kBufferTypeDepth ||
             resources[i].type == sl::kBufferTypeHiResDepth || resources[i].type == sl::kBufferTypeLinearDepth ||
             resources[i].type == sl::kBufferTypeMotionVectors || resources[i].type == sl::kBufferTypeUIColorAndAlpha ||
             resources[i].type == sl::kBufferTypeBidirectionalDistortionField))
        {
            State::Instance().slFGInputs.reportResource(resources[i], cmdBuffer, (uint32_t) frame);
        }
        else if (State::Instance().activeFgInput == FGInput::NvngxFG)
        {
            LOG_TRACE("Tagging resource of type: {}", magic_enum::enum_name(typeEnum));
        }
    }

    auto result = o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslEvaluateFeature(sl::Feature feature, const sl::FrameToken& frame,
                                                const sl::BaseStructure** inputs, uint32_t numInputs,
                                                sl::CommandBuffer* cmdBuffer)
{
    static std::atomic_bool nullTokenLogged = false;
    if (NullFrameToken(frame, __FUNCTION__, nullTokenLogged))
        return o_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);

    LOG_DEBUG("frameIndex: {}", static_cast<uint32_t>(frame));

    if (State::Instance().activeFgInput == FGInput::DLSSG && numInputs > 0 && inputs != nullptr)
    {
        for (uint32_t i = 0; i < numInputs; i++)
        {
            if (inputs[i] == nullptr)
                continue;

            if (inputs[i]->structType == sl::ResourceTag::s_structType)
            {
                auto tag = (const sl::ResourceTag*) inputs[i];

                if (tag->type == sl::kBufferTypeHUDLessColor || tag->type == sl::kBufferTypeDepth ||
                    tag->type == sl::kBufferTypeHiResDepth || tag->type == sl::kBufferTypeLinearDepth ||
                    tag->type == sl::kBufferTypeMotionVectors || tag->type == sl::kBufferTypeUIColorAndAlpha ||
                    tag->type == sl::kBufferTypeBidirectionalDistortionField)
                {
                    State::Instance().slFGInputs.reportResource(*tag, cmdBuffer, (uint32_t) frame);
                }
            }
        }
    }

    auto result = o_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);
    return result;
}

sl::Result StreamlineHooks::hkslAllocateResources(sl::CommandBuffer* cmdBuffer, sl::Feature feature,
                                                  const sl::ViewportHandle& viewport)
{
    LOG_FUNC();
    auto result = o_slAllocateResources(cmdBuffer, feature, viewport);
    return result;
}

sl::Result StreamlineHooks::hkslGetNativeInterface(void* proxyInterface, void** baseInterface)
{
    LOG_FUNC();
    auto result = o_slGetNativeInterface(proxyInterface, baseInterface);
    return result;
}

sl::Result StreamlineHooks::hkslSetD3DDevice(void* d3dDevice)
{
    LOG_FUNC();
    auto result = o_slSetD3DDevice(d3dDevice);
    return result;
}

void StreamlineHooks::streamlineLogCallback_sl1(sl1::LogType type, const char* msg)
{
    if (msg == nullptr)
        return;

    char* trimmed_msg = trimStreamlineLog(msg);

    if (trimmed_msg != nullptr)
    {
        switch (type)
        {
        case sl1::LogType::eLogTypeWarn:
            LOG_WARN("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeInfo:
            LOG_INFO("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeError:
            LOG_ERROR("{}", trimmed_msg);
            break;
        case sl1::LogType::eLogTypeCount:
            LOG_ERROR("{}", trimmed_msg);
            break;
        }

        free(trimmed_msg);
    }

    if (o_logCallback_sl1)
        o_logCallback_sl1(type, msg);
}

bool StreamlineHooks::hkslInit_sl1(const sl1::Preferences& pref, int applicationId)
{
    LOG_FUNC();

    if (Util::GetCallerModule(_ReturnAddress()) != Util::GetCallerModule((void*) &StreamlineHooks::hkslInit_sl1))
        State::Instance().gameCalledSlInit = true;

    sl1::Preferences localPref = pref;

    if (localPref.logMessageCallback != &streamlineLogCallback_sl1)
        o_logCallback_sl1 = localPref.logMessageCallback;
    localPref.logLevel = sl1::LogLevel::eLogLevelCount;
    localPref.logMessageCallback = &streamlineLogCallback_sl1;
    return o_slInit_sl1(localPref, applicationId);
}

bool StreamlineHooks::hkslSetTag_sl1(const sl1::Resource* resource, sl1::BufferType tag, uint32_t id,
                                     const sl1::Extent* extent)
{
    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setTag(resource, tag, id, extent);

    return o_slSetTag_sl1(resource, tag, id, extent);
}

bool StreamlineHooks::hkslSetConstants_sl1(const sl1::Constants& values, uint32_t frameIndex, uint32_t id)
{
    std::scoped_lock lock(setConstantsMutex);

    LOG_TRACE("SL1 slSetConstants frameIndex: {}, id: {}", frameIndex, id);

    if (IsSL1AndFGActive())
        State::Instance().s_sl1FGInputs.setConstants(values, frameIndex, id);

    return o_slSetConstants_interposer_sl1(values, frameIndex, id);
}

bool StreamlineHooks::hkslEvaluateFeature_sl1(sl1::CommandBuffer* cmdBuffer, sl1::Feature feature, uint32_t frameIndex,
                                              uint32_t id)
{
    LOG_TRACE("SL1 slEvaluateFeature feature: {}, frameIndex: {}, id: {}", magic_enum::enum_name(feature), frameIndex,
              id);

    if (IsSL1AndFGActive() && feature == sl1::Feature::eFeatureReflex)
    {
        const auto marker = (sl1::ReflexMarker) id;

        if (marker == sl1::ReflexMarker::eReflexMarkerRenderSubmitStart)
        {
            State::Instance().s_sl1FGInputs.evaluateState();
            State::Instance().s_sl1FGInputs.evaluateFeature(cmdBuffer, feature, frameIndex, id);
        }
        else if (marker == sl1::ReflexMarker::eReflexMarkerPresentStart)
        {
            State::Instance().s_sl1FGInputs.markPresent(frameIndex);
        }
    }

    return o_slEvaluateFeature_sl1(cmdBuffer, feature, frameIndex, id);
}

void StreamlineHooks::hookSystemCaps(sl::param::IParameters* params)
{
    if (State::Instance().streamlineVersion.major > 1)
    {
        if (!systemCaps)
            sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &systemCaps);
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        // This should be Streamline 1.5 as previous versions don't even have slOnPluginLoad
        if (!systemCapsSl15)
        {
            LOG_TRACE(
                "Attempting to get system caps for Streamline v1, this could fail depending on the exact version");
            sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &systemCapsSl15);
        }
    }
}

uint32_t StreamlineHooks::getSystemCapsArch(SystemCaps* altSystemCaps)
{
    uint32_t highestArch = 0;

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (!fakenvapi::isUsingAsMainNvapi() && primaryGpu.vendorId == VendorId::Nvidia)
    {
        if (State::Instance().streamlineVersion.major > 1)
        {
            auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
            if (caps)
            {
                for (auto& adapter : caps->adapters)
                {
                    if (adapter.architecture > highestArch)
                        highestArch = adapter.architecture;
                }
            }
        }
        else if (State::Instance().streamlineVersion.major == 1)
        {
            if (systemCapsSl15)
            {
                for (uint32_t i = 0; i < systemCapsSl15->gpuCount; i++)
                {
                    if (systemCapsSl15->architecture[i] > highestArch)
                        highestArch = systemCapsSl15->architecture[i];
                }
            }
        }
    }

    // By default spoof Pascal, gets Reflex but not DLSSD
    // Could be problematic if not using fakenvapi but nvapi might not be initialized yet
    if (highestArch == 0)
        highestArch = NV_GPU_ARCHITECTURE_GP100;

    return highestArch;
}

void StreamlineHooks::setArch(uint32_t arch, SystemCaps* altSystemCaps)
{
    auto primaryGpu = IdentifyGpu::getPrimaryGpu();

    // altSystemCaps has to be sl2+
    if (State::Instance().streamlineVersion.major > 1 || altSystemCaps)
    {
        // Assumes that altCaps are always for SL2+
        auto caps = altSystemCaps != nullptr ? altSystemCaps : systemCaps;
        if (caps)
        {
            for (uint32_t i = 0; i < caps->gpuCount; i++)
            {
                caps->adapters[i].architecture = arch;
                caps->adapters[i].vendor = VendorId::Nvidia;
            }

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                caps->driverVersionMajor = 999;

            caps->hwsSupported = true;
        }
    }
    else if (State::Instance().streamlineVersion.major == 1)
    {
        if (systemCapsSl15)
        {
            for (uint32_t i = 0; i < systemCapsSl15->gpuCount; i++)
                systemCapsSl15->architecture[i] = arch;

            if (fakenvapi::isUsingAsMainNvapi() || primaryGpu.vendorId != VendorId::Nvidia)
                systemCapsSl15->driverVersionMajor = 999;

            systemCapsSl15->hwSchedulingEnabled = true;
        }
    }
}

// Spoof arch based on feature and current arch
void StreamlineHooks::spoofArch(uint32_t currentArch, sl::Feature feature, SystemCaps* altSystemCaps)
{
    constexpr uint32_t maxArch = 0xFFFFFFFF;

    // Don't change arch for DLSS/DLSSD with turing and above
    if (feature == sl::kFeatureDLSS)
    {
        if (currentArch < NV_GPU_ARCHITECTURE_TU100)
            return setArch(maxArch, altSystemCaps);
    }

    // Don't spoof DLSSD at all
    else if (feature == sl::kFeatureDLSS_RR)
    {
        return;
    }

    // Don't change arch for DLSSG with ada and above
    else if (feature == sl::kFeatureDLSS_G)
    {
        if (State::Instance().activeFgNvngx != FGNvngxReplacement::None)
        {
            if (!Nvngx_FG::isDx12Available() && !Nvngx_FG::isVulkanAvailable())
                return setArch(0);
        }

        if (currentArch < NV_GPU_ARCHITECTURE_AD100)
            return setArch(maxArch, altSystemCaps);
    }

    else if (feature == sl::kFeatureReflex || feature == sl::kFeaturePCL)
    {
        if (fakenvapi::isUsingAsMainNvapi())
            return setArch(maxArch, altSystemCaps);
    }
}

bool StreamlineHooks::hkdlss_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                            const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS);
    }

    auto result = o_dlss_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGetOptimalSettings(const sl::DLSSOptions& options,
                                                       sl::DLSSOptimalSettings& settings)
{
    static bool modesBroken = false;

    auto localOptions = options;

    if (localOptions.mode == sl::DLSSMode::eOff)
        modesBroken = true;

    if (modesBroken)
    {
        if (localOptions.mode == sl::DLSSMode::eMaxPerformance)
            localOptions.mode = sl::DLSSMode::eUltraPerformance;
        else if (localOptions.mode == sl::DLSSMode::eBalanced)
            localOptions.mode = sl::DLSSMode::eMaxPerformance;
        else if (localOptions.mode == sl::DLSSMode::eMaxQuality)
            localOptions.mode = sl::DLSSMode::eBalanced;
        else if (localOptions.mode == sl::DLSSMode::eUltraQuality)
            localOptions.mode = sl::DLSSMode::eMaxQuality;
        else if (localOptions.mode == sl::DLSSMode::eUltraPerformance)
            localOptions.mode = sl::DLSSMode::eDLAA;
    }

    return o_slDLSSGetOptimalSettings(localOptions, settings);
}

bool StreamlineHooks::hkdlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                             const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    bool shouldSpoofArch =
        Config::Instance()->StreamlineSpoofing.value_or_default() &&
        (State::Instance().activeFgInput == FGInput::NvngxFG || State::Instance().activeFgInput == FGInput::DLSSG);

    uint32_t currentArch = 0;
    if (shouldSpoofArch)
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureDLSS_G);
    }

    auto result = o_dlssg_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (shouldSpoofArch)
        setArch(currentArch);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    // Kill the DLSSG streamline swapchain hooks
    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgOutput == FGOutput::DLSSG)
    {
        if (configJson.contains("/hooks"_json_pointer))
            configJson["hooks"].clear();

        if (configJson.contains("/exclusive_hooks"_json_pointer))
            configJson["exclusive_hooks"].clear();

        if (configJson.contains("/external/feature/tags"_json_pointer))
            configJson["external"]["feature"]["tags"].clear(); // We handle the DLSSG resources

        if (configJson.contains("/external/vk/device/queues/compute/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["compute"]["count"] = 0;

        if (configJson.contains("/external/vk/device/queues/graphics/count"_json_pointer))
            configJson["external"]["vk"]["device"]["queues"]["graphics"]["count"] = 0;

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG || State::Instance().activeFgInput == FGInput::NvngxFG)
    {
        if (configJson.contains("/vsync/supported"_json_pointer))
            configJson["vsync"]["supported"] = true; // disable eVSyncOffRequired

        if (configJson.contains("/external/hws/required"_json_pointer))
            configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

        // if (configJson.contains("/external/vk/opticalflow/supported"_json_pointer))
        //     configJson["external"]["vk"]["opticalflow"]["supported"] = true;
    }

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

const char* StreamlineHooks::hkdlssg_slGetPluginJSONConfig_sl1()
{
    static std::string patchedConfig;

    const char* originalConfig = o_dlssg_slGetPluginJSONConfig_sl1();

    if (originalConfig == nullptr)
        return originalConfig;

    try
    {
        auto configJson = nlohmann::json::parse(originalConfig);

        LOG_DEBUG("SL1 DLSSG JSON before patch: {}", configJson.dump());

        PatchSL1PluginJson(configJson);
        // RemoveSL1DLSSGHookEntriesRecursive(configJson);

        patchedConfig = configJson.dump();

        LOG_DEBUG("SL1 DLSSG JSON after patch: {}", patchedConfig);

        return patchedConfig.c_str();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Failed to patch SL1 DLSSG JSON config: {}", e.what());
        return originalConfig;
    }
}

bool StreamlineHooks::hklocal_dlssg_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                                   const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    bool shouldSpoofArch = Config::Instance()->StreamlineSpoofing.value_or_default();

    uint32_t currentArch = 0;
    SystemCaps* localSystemCaps = nullptr;
    if (shouldSpoofArch)
    {
        sl::param::getPointerParam(params, sl::param::common::kSystemCaps, &localSystemCaps);

        if (localSystemCaps)
        {
            currentArch = getSystemCapsArch(localSystemCaps);
            spoofArch(currentArch, sl::kFeatureDLSS_G, localSystemCaps);
        }
    }

    auto result = o_local_dlssg_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (shouldSpoofArch && localSystemCaps)
        setArch(currentArch, localSystemCaps);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    if (configJson.contains("/external/hws/required"_json_pointer))
        configJson["external"]["hws"]["required"] = false; // disable eHardwareSchedulingRequired

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslSetConstants(const sl::Constants& values, const sl::FrameToken& frame,
                                             const sl::ViewportHandle& viewport)
{
    static std::atomic_bool nullTokenLogged = false;
    if (NullFrameToken(frame, __FUNCTION__, nullTokenLogged))
        return o_slSetConstants(values, frame, viewport);

    std::scoped_lock lock(setConstantsMutex);
    LOG_TRACE("called with frameIndex: {}, viewport: {}", (unsigned int) frame, (unsigned int) viewport);

    State::Instance().slFGInputs.setConstants(values, (uint32_t) frame);

    return o_slSetConstants(values, frame, viewport);
}

bool StreamlineHooks::hkcommon_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    auto result = o_common_slOnPluginLoad(params, loaderJSON, pluginJSON);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    auto& slVersion = State::Instance().streamlineVersion;

    // Grab a version of the potentially updated sl.common
    // Opti assumes that all plugins will have this version
    configJson.at("version").at("major").get_to(slVersion.major);
    configJson.at("version").at("minor").get_to(slVersion.minor);
    configJson.at("version").at("build").get_to(slVersion.patch);

    // Completely disables Streamline hooks
    // if (true)
    //    configJson["hooks"].clear();
    //    configJson["exclusive_hooks"].clear();
    //}

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options)
{
    lastDlssgViewport = viewport;
    lastDlssgOptions = options;

    // Avoid reading past the game's struct's size
    sl::DLSSGOptions newOptions {};
    auto newStructVer = newOptions.structVersion;

    if (options.structVersion == 1)
        memcpy(&newOptions, &options, 104);
    else if (options.structVersion == 2 || options.structVersion == 3)
        memcpy(&newOptions, &options, 112);
    else if (options.structVersion == 4 || options.structVersion == 5)
        memcpy(&newOptions, &options, 120);
    else
        newOptions = options;

    newOptions.structVersion = newStructVer;

    // What the game asked for, before any override. A struct too old to carry the field reads as 1 (2X).
    const unsigned int requestedCount = newOptions.numFramesToGenerate;

    auto& state = State::Instance();

    // A game that reached the real plugin's SetOptions (not our slGetFeatureFunction stand-in) is followed the same way
    if (state.activeFgInput == FGInput::DLSSG)
        RecordGameDlssgRequest(options);

    // Disable game's DLSSG when we are trying to create our own instance of DLSSG
    if (state.activeFgInput != FGInput::DLSSG && state.activeFgOutput == FGOutput::DLSSG)
    {
        newOptions.mode = sl::DLSSGMode::eOff;
        return o_slDLSSGSetOptions(viewport, newOptions);
    }

    // Make DLSSG auto always mean On
    if (newOptions.mode == sl::DLSSGMode::eAuto)
        newOptions.mode = sl::DLSSGMode::eOn;

    const auto dlssgPotentiallyActive = newOptions.mode == sl::DLSSGMode::eOn ||
                                        newOptions.mode == sl::DLSSGMode::eAuto ||
                                        newOptions.mode == sl::DLSSGMode::eDynamic;

    bool enableDynamicMode = Config::Instance()->FGDLSSGOverrideForceDMFG.value_or_default() &&
                             state.dlssgGameDMFGSupported && dlssgPotentiallyActive;

    if (enableDynamicMode)
    {
        newOptions.mode = sl::DLSSGMode::eDynamic;
    }

    if (newOptions.mode == sl::DLSSGMode::eDynamic && Config::Instance()->FGDLSSGFramerateTargetDMFG.has_value())
    {
        newOptions.dynamicTargetFrameRate = Config::Instance()->FGDLSSGFramerateTargetDMFG.value();
    }

    applyMenuDlssgInterlock(newOptions, dlssgPotentiallyActive);

    LOG_TRACE("DLSSG Modified Mode: {}", magic_enum::enum_name(newOptions.mode));

    if (dlssgPotentiallyActive && state.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        // Populate dlssgMfgMax once
        if (!state.dlssgMfgMax.has_value())
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (o_slDLSSGGetState(viewport, localState, &localOptions) == sl::Result::eOk)
            {
                if (localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
                {
                    state.dlssgMfgMax = localState.numFramesToGenerateMax;
                    LOG_TRACE("Saving original numFramesToGenerateMax: {}", state.dlssgMfgMax.value());

                    // Volatile: the clamp holds for this run and leaves the ini setting alone.
                    if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                        Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > state.dlssgMfgMax.value())
                    {
                        Config::Instance()->FGDLSSGOverrideInterpolationCount.set_volatile_value(
                            state.dlssgMfgMax.value());
                    }
                }
            }
        }

        // Won't take effect with Dynamic
        if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value())
        {
            auto overrideCount = Config::Instance()->FGDLSSGOverrideInterpolationCount.value();
            if (overrideCount != 0)
                newOptions.numFramesToGenerate = overrideCount;
            else if (!enableDynamicMode)
                newOptions.mode = sl::DLSSGMode::eOff;
        }
    }
    else if (dlssgPotentiallyActive && Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
             Config::Instance()->FGDLSSGOverrideInterpolationCount.value() != 0)
    {
        // Once. slDLSSGSetOptions runs per frame.
        static bool warnedNoMfg = false;

        if (!warnedNoMfg)
        {
            warnedNoMfg = true;
            LOG_WARN("Interpolation count override ignored: Streamline {}.{}.{}, multi frame generation "
                     "needs 2.7.1",
                     state.streamlineVersion.major, state.streamlineVersion.minor, state.streamlineVersion.patch);
        }
    }

    state.dlssgLastSetMode = newOptions.mode;

    const auto result = o_slDLSSGSetOptions(viewport, newOptions);

    DlssgTelemetry::RecordSetOptions(requestedCount, newOptions.numFramesToGenerate,
                                     newOptions.mode != sl::DLSSGMode::eOff, static_cast<unsigned int>(result));

    return result;
}

sl::Result StreamlineHooks::hkslDLSSGGetState(const sl::ViewportHandle& viewport, sl::DLSSGState& state,
                                              const sl::DLSSGOptions* options)
{
    sl::Result result {};

    const auto originalStructVersion = state.structVersion;
    if (originalStructVersion < 4)
    {
        sl::DLSSGState newState {};

        // We might be feeding a newer struct to an older SL but that seems to work just fine for this Get function
        result = o_slDLSSGGetState(viewport, dynamic_cast<sl::DLSSGState&>(newState), options);
        if (result != sl::Result::eOk)
            return result;

        // Copy back data to game's struct
        memcpy(&state, &newState, 56); // struct ver 1 size
        state.structVersion = originalStructVersion;

        if (originalStructVersion >= 2)
        {
            state.numFramesToGenerateMax = newState.numFramesToGenerateMax;
            state.bReserved4 = newState.bReserved4;
            state.bIsVsyncSupportAvailable = newState.bIsVsyncSupportAvailable;
        }

        if (originalStructVersion >= 3)
        {
            state.inputsProcessingCompletionFence = newState.inputsProcessingCompletionFence;
            state.lastPresentInputsProcessingCompletionFenceValue =
                newState.lastPresentInputsProcessingCompletionFenceValue;
        }

        State::Instance().dlssgGameDMFGSupported = newState.bIsDynamicMFGSupported == sl::eTrue;

        // The real DLSS-G's count, unless our own frame generation stands in for it (it writes its own
        // count further down).
        if (State::Instance().activeFgInput != FGInput::DLSSG)
            DlssgTelemetry::RecordState(newState.numFramesActuallyPresented);
    }
    else
    {
        result = o_slDLSSGGetState(viewport, state, options);
        if (result != sl::Result::eOk)
            return result;
        State::Instance().dlssgGameDMFGSupported = state.bIsDynamicMFGSupported == sl::eTrue;

        if (State::Instance().activeFgInput != FGInput::DLSSG)
            DlssgTelemetry::RecordState(state.numFramesActuallyPresented);
    }

    if (!State::Instance().dlssgGameDMFGSupported)
    {
        Config::Instance()->FGDLSSGOverrideForceDMFG.set_volatile_value(false);
    }

    auto& optiState = State::Instance();

    if (optiState.streamlineVersion >= feature_version { 2, 7, 1 })
    {
        if (!optiState.dlssgMfgMax.has_value())
        {
            sl::DLSSGState localState {};
            sl::DLSSGOptions localOptions {};
            if (o_slDLSSGGetState(viewport, localState, &localOptions) == sl::Result::eOk)
            {
                if (localState.numFramesToGenerateMax > 0 && localState.numFramesToGenerateMax < 6)
                {
                    optiState.dlssgMfgMax = localState.numFramesToGenerateMax;
                    LOG_TRACE("Saving original numFramesToGenerateMax: {}", optiState.dlssgMfgMax.value());

                    // Volatile: the clamp holds for this run and leaves the ini setting alone.
                    if (Config::Instance()->FGDLSSGOverrideInterpolationCount.has_value() &&
                        Config::Instance()->FGDLSSGOverrideInterpolationCount.value() > optiState.dlssgMfgMax.value())
                    {
                        Config::Instance()->FGDLSSGOverrideInterpolationCount.set_volatile_value(
                            optiState.dlssgMfgMax.value());
                    }
                }
            }
        }
    }

    if (optiState.activeFgInput == FGInput::DLSSG)
    {
        if (optiState.currentFG != nullptr && options != nullptr &&
            options->flags & sl::DLSSGFlags::eRequestVRAMEstimate)
            state.estimatedVRAMUsageInBytes = static_cast<uint64_t>(256 * 1024) * 1024;

        FillDlssgInputState(state);

        LOG_DEBUG("Status: {}, numFramesActuallyPresented: {}", magic_enum::enum_name(state.status),
                  state.numFramesActuallyPresented);
    }

    return result;
}

bool StreamlineHooks::hkreflex_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                              const char** pluginJSON)
{
    LOG_FUNC();

    // TODO: do it better than "static" and hoping for the best
    static std::string config;

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeatureReflex);
    }

    auto result = o_reflex_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    nlohmann::json configJson = nlohmann::json::parse(*pluginJSON);

    auto primaryGpu = IdentifyGpu::getPrimaryGpu();
    if (primaryGpu.vendorId != VendorId::Nvidia || !primaryGpu.dlssCapable)
    {
        if (configJson.contains("/external/vk/instance/extensions"_json_pointer))
            configJson["external"]["vk"]["instance"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/extensions"_json_pointer))
            configJson["external"]["vk"]["device"]["extensions"].clear();

        if (configJson.contains("/external/vk/device/1.2_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.2_features"].clear();

        if (configJson.contains("/external/vk/device/1.3_features"_json_pointer))
            configJson["external"]["vk"]["device"]["1.3_features"].clear();
    }

    PatchSL1PluginJson(configJson);

    config = configJson.dump();

    *pluginJSON = config.c_str();

    return result;
}

sl::Result StreamlineHooks::hkslReflexSetOptions(const sl::ReflexOptions& options)
{
    reflexGamesLastMode = options.mode;

    sl::ReflexOptions newOptions = options;

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        newOptions.mode = sl::ReflexMode::eLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // if (Config::Instance()->FN_ForceReflex == 1)
    //     newOptions.mode = sl::ReflexMode::eOff;

    return o_slReflexSetOptions(newOptions);
}

sl::Result StreamlineHooks::hkslReflexSleep(const sl::FrameToken& frame)
{
    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::ReflexSleep()(frame);
    // }

    return o_slReflexSleep(frame);
}

void* StreamlineHooks::hkdlss_slGetPluginFunction(const char* functionName)
{
    LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_dlss_slOnPluginLoad = (PFN_slOnPluginLoad) LoadOriginal(o_dlss_slGetPluginFunction)(functionName);
        return &hkdlss_slOnPluginLoad;
    }

    if (strcmp(functionName, "slDLSSGetOptimalSettings") == 0 &&
        State::Instance().gameQuirks & GameQuirk::PregmataFixDLSSModes)
    {
        o_slDLSSGetOptimalSettings = (decltype(&slDLSSGetOptimalSettings)) LoadOriginal(o_dlss_slGetPluginFunction)(functionName);
        return &hkslDLSSGetOptimalSettings;
    }

    return LoadOriginal(o_dlss_slGetPluginFunction)(functionName);
}

void* StreamlineHooks::hkdlssg_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_dlssg_slOnPluginLoad = (PFN_slOnPluginLoad) LoadOriginal(o_dlssg_slGetPluginFunction)(functionName);
        return &hkdlssg_slOnPluginLoad;
    }

    if (strcmp(functionName, "slDLSSGSetOptions") == 0)
    {
        o_slDLSSGSetOptions = (decltype(&slDLSSGSetOptions)) LoadOriginal(o_dlssg_slGetPluginFunction)(functionName);

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(_ReturnAddress()); callerModule == steamOverlay)
                return o_slDLSSGSetOptions;
        }

        return &hkslDLSSGSetOptions;
    }

    if (strcmp(functionName, "slDLSSGGetState") == 0)
    {
        o_slDLSSGGetState = (decltype(&slDLSSGGetState)) LoadOriginal(o_dlssg_slGetPluginFunction)(functionName);

        // Give steam overlay the original as it seems to be hooking it
        auto steamOverlay = KernelBaseProxy::GetModuleHandleA_()("gameoverlayrenderer64.dll");
        if (steamOverlay != nullptr)
        {
            if (HMODULE callerModule = Util::GetCallerModule(_ReturnAddress()); callerModule == steamOverlay)
                return o_slDLSSGGetState;
        }

        return &hkslDLSSGGetState;
    }

    if (strcmp(functionName, "slGetPluginJSONConfig") == 0 && IsSL1AndDLSSGManaged())
    {
        o_dlssg_slGetPluginJSONConfig_sl1 =
            reinterpret_cast<PFN_slGetPluginJSONConfig_sl1>(LoadOriginal(o_dlssg_slGetPluginFunction)(functionName));

        if (o_dlssg_slGetPluginJSONConfig_sl1 != nullptr)
        {
            LOG_WARN("Hooking SL1 DLSSG slGetPluginJSONConfig");
            return &hkdlssg_slGetPluginJSONConfig_sl1;
        }
    }

    // Ensure that we have those DLSSG calls
    if (!o_slDLSSGSetOptions)
        o_slDLSSGSetOptions = (decltype(&slDLSSGSetOptions)) LoadOriginal(o_dlssg_slGetPluginFunction)("slDLSSGSetOptions");

    if (!o_slDLSSGGetState)
        o_slDLSSGGetState = (decltype(&slDLSSGGetState)) LoadOriginal(o_dlssg_slGetPluginFunction)("slDLSSGGetState");

    if (auto* wrapped = DlssNr::StreamlinePicture::Wrap(functionName, LoadOriginal(o_dlssg_slGetPluginFunction)))
        return wrapped;

    return LoadOriginal(o_dlssg_slGetPluginFunction)(functionName);
}

void* StreamlineHooks::hklocal_dlssg_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0 && State::Instance().activeFgNvngx != FGNvngxReplacement::None)
    {
        o_local_dlssg_slOnPluginLoad = (PFN_slOnPluginLoad) LoadOriginal(o_local_dlssg_slGetPluginFunction)(functionName);
        return &hklocal_dlssg_slOnPluginLoad;
    }

    if (auto* wrapped =
            DlssNr::StreamlinePicture::Wrap(functionName, LoadOriginal(o_local_dlssg_slGetPluginFunction), true))
        return wrapped;

    return LoadOriginal(o_local_dlssg_slGetPluginFunction)(functionName);
}

bool StreamlineHooks::hkreflex_slSetConstants_sl1(const void* data, uint32_t frameIndex, uint32_t id)
{
    // Streamline v1's version of slReflexSetOptions + slPCLSetMarker
    static sl1::ReflexConstants constants {};
    constants = *(const sl1::ReflexConstants*) data;

    reflexGamesLastMode = (sl::ReflexMode) constants.mode;

    LOG_DEBUG("mode: {}, frameIndex: {}, id: {}", (uint32_t) constants.mode, frameIndex, id);

    if (Config::Instance()->FN_ForceReflex == ForceReflex::ForceEnable)
        constants.mode = sl1::ReflexMode::eReflexModeLowLatencyWithBoost;

    // Will cause a pink screen when used with DLSSG
    // else if (Config::Instance()->FN_ForceReflex == 1)
    //     constants.mode = sl1::ReflexMode::eReflexModeOff;

    return o_reflex_slSetConstants_sl1(&constants, frameIndex, id);
}

void* StreamlineHooks::hkreflex_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slSetConstants") == 0 && State::Instance().streamlineVersion.major == 1)
    {
        o_reflex_slSetConstants_sl1 = (PFN_slSetConstants_sl1) LoadOriginal(o_reflex_slGetPluginFunction)(functionName);
        return &hkreflex_slSetConstants_sl1;
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_reflex_slOnPluginLoad = (PFN_slOnPluginLoad) LoadOriginal(o_reflex_slGetPluginFunction)(functionName);
        return &hkreflex_slOnPluginLoad;
    }

    if (strcmp(functionName, "slReflexSetOptions") == 0)
    {
        o_slReflexSetOptions = (decltype(&slReflexSetOptions)) LoadOriginal(o_reflex_slGetPluginFunction)(functionName);
        return &hkslReflexSetOptions;
    }

    if (strcmp(functionName, "slReflexSleep") == 0)
    {
        o_slReflexSleep = (decltype(&slReflexSleep)) LoadOriginal(o_reflex_slGetPluginFunction)(functionName);
        return &hkslReflexSleep;
    }

    // TODO: Hopefully a game doesn't call both, maybe separate
    if (strcmp(functionName, "slReflexSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        return HookPCLSetMarker(LoadOriginal(o_reflex_slGetPluginFunction)(functionName));
    }

    return LoadOriginal(o_reflex_slGetPluginFunction)(functionName);
}

// The reflex and the PCL plugin can both be asked for the marker function. Hooked only when Streamline gives one: a
// null answer must not replace one found earlier, and the game then gets Streamline's null, not a hook with nothing
// behind it
void* StreamlineHooks::HookPCLSetMarker(void* original)
{
    if (original == nullptr)
    {
        LOG_WARN("Streamline has no marker function here, not hooked");
        return nullptr;
    }

    o_slPCLSetMarker = (decltype(&slPCLSetMarker)) original;
    return &hkslPCLSetMarker;
}

sl::Result StreamlineHooks::hkslPCLSetMarker(sl::PCLMarker marker, const sl::FrameToken& frame)
{
    if (o_slPCLSetMarker == nullptr)
        return sl::Result::eErrorFeatureMissing;

    static std::atomic_bool nullTokenLogged = false;
    if (NullFrameToken(frame, __FUNCTION__, nullTokenLogged))
        return o_slPCLSetMarker(marker, frame);

    // if (State::Instance().activeFgOutput == FGOutput::DLSSG && StreamlineProxy::IsD3D12Inited() &&
    //     Config::Instance()->FGDLSSGUseGamesReflexMarkers.value_or_default())
    //{
    //     return StreamlineProxy::PCLSetMarker()(marker, frame);
    // }

    // HACK for broken games
    if (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers)
    {
        static uint64_t last_simulation_end_id = 0;
        if (marker == sl::PCLMarker::eSimulationEnd)
        {
            last_simulation_end_id = frame;
        }

        if (marker == sl::PCLMarker::eSimulationStart && last_simulation_end_id >= frame && o_slGetNewFrameToken)
        {
            const uint64_t correction_offset = last_simulation_end_id - frame + 1;
            uint32_t newFrameId = static_cast<uint32_t>(frame + correction_offset);

            sl::FrameToken* newFramePointer {};
            auto result = o_slGetNewFrameToken(newFramePointer, &newFrameId);

            LOG_WARN("Simulation start marker sent after end marker, offset: {}", correction_offset);

            result = o_slPCLSetMarker(marker, *newFramePointer);
            return result;
        }
    }

    if (State::Instance().activeFgInput == FGInput::DLSSG)
    {
        if (State::Instance().streamlineVersion.major == 1)
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().s_sl1FGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().s_sl1FGInputs.markPresent(frame);
            }
        }
        else
        {
            if (marker == sl::PCLMarker::eRenderSubmitStart)
            {
                State::Instance().slFGInputs.evaluateState();
            }
            else if (marker == sl::PCLMarker::ePresentStart)
            {
                State::Instance().slFGInputs.markPresent(frame);
            }
        }
    }

    return o_slPCLSetMarker(marker, frame);
}

bool StreamlineHooks::hkpcl_slOnPluginLoad(sl::param::IParameters* params, const char* loaderJSON,
                                           const char** pluginJSON)
{
    LOG_FUNC();

    uint32_t currentArch = 0;
    if (Config::Instance()->StreamlineSpoofing.value_or_default())
    {
        hookSystemCaps(params);
        currentArch = getSystemCapsArch();
        spoofArch(currentArch, sl::kFeaturePCL);
    }

    auto result = o_pcl_slOnPluginLoad(params, loaderJSON, pluginJSON);

    if (Config::Instance()->StreamlineSpoofing.value_or_default())
        setArch(currentArch);

    return result;
}

void* StreamlineHooks::hkpcl_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slPCLSetMarker") == 0 &&
        (State::Instance().gameQuirks & GameQuirk::FixSlSimulationMarkers ||
         State::Instance().activeFgInput == FGInput::DLSSG))
    {
        return HookPCLSetMarker(LoadOriginal(o_pcl_slGetPluginFunction)(functionName));
    }

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_pcl_slOnPluginLoad = (PFN_slOnPluginLoad) LoadOriginal(o_pcl_slGetPluginFunction)(functionName);
        return &hkpcl_slOnPluginLoad;
    }

    return LoadOriginal(o_pcl_slGetPluginFunction)(functionName);
}

bool StreamlineHooks::hk_setVoid(void* self, const char* key, void** value)
{
    // LOG_DEBUG("{}", key);

    if (strcmp(key, sl::param::common::kSystemCaps) == 0)
    {
        LOG_TRACE("Attempting to change system caps for Streamline v1, this could fail depending on the exact version");

        // SystemCapsSl15 is not entirely correct for Streamline 1.3
        // But we here only use the beginning that matches + extra
        auto caps = (SystemCapsSl15*) value;

        if (caps)
        {
            caps->gpuCount = 1;
            caps->architecture[0] = UINT_MAX;
            caps->driverVersionMajor = 999;

            // HAGS
            *((char*) value + 56) = (char) 0x01;
        }
    }

    return o_setVoid(self, key, value);
}

void StreamlineHooks::hkcommon_slSetParameters_sl1(void* params)
{
    LOG_FUNC();

    if (o_setVoid == nullptr && params)
    {
        void** vtable = *(void***) params;

        // It's flipped, 0 -> set void*, 7 -> get void*
        o_setVoid = (PFN_setVoid) vtable[0];

        std::lock_guard<std::mutex> detoursLock(g_detoursTransactionMutex);
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        if (o_setVoid != nullptr)
            DetourAttach(&(PVOID&) o_setVoid, hk_setVoid);

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to hook setVoid: {:X}", detourResult);
            o_setVoid = nullptr;
        }
    }

    o_common_slSetParameters_sl1(params);
}

void* StreamlineHooks::hkcommon_slGetPluginFunction(const char* functionName)
{
    // LOG_DEBUG("{}", functionName);

    if (strcmp(functionName, "slOnPluginLoad") == 0)
    {
        o_common_slOnPluginLoad = (PFN_slOnPluginLoad) LoadOriginal(o_common_slGetPluginFunction)(functionName);
        return &hkcommon_slOnPluginLoad;
    }

    // Used around Streamline v1.3, as 1.5 doesn't seem to have it anymore
    if (strcmp(functionName, "slSetParameters") == 0)
    {
        o_common_slSetParameters_sl1 = (PFN_slSetParameters_sl1) LoadOriginal(o_common_slGetPluginFunction)(functionName);
        return &hkcommon_slSetParameters_sl1;
    }

    return LoadOriginal(o_common_slGetPluginFunction)(functionName);
}

void StreamlineHooks::updateForceReflex()
{
    // Not needed for Streamline v1 as slSetConstants is sent every frame
    if (o_slReflexSetOptions)
    {
        sl::ReflexOptions options;

        auto forceReflex = Config::Instance()->FN_ForceReflex.value_or_default();

        if (forceReflex == ForceReflex::ForceEnable)
            options.mode = sl::ReflexMode::eLowLatencyWithBoost;
        else if (forceReflex == ForceReflex::ForceDisable)
            options.mode = sl::ReflexMode::eOff;
        else if (forceReflex == ForceReflex::InGame)
            options.mode = reflexGamesLastMode;

        auto result = o_slReflexSetOptions(options);
        if (result != sl::Result::eOk)
        {
            LOG_WARN("Failed to update Reflex mode with error code: {} ({:X})", magic_enum::enum_name(result),
                     (UINT) result);
        }
    }
}

void StreamlineHooks::updateDlssgOptions()
{
    if (o_slDLSSGSetOptions)
    {
        LOG_FUNC();
        hkslDLSSGSetOptions(lastDlssgViewport, lastDlssgOptions);
    }
}

void StreamlineHooks::applyMenuDlssgInterlock(sl::DLSSGOptions& options, bool potentiallyActive)
{
    auto& state = State::Instance();
    if (state.externalFrameGeneration || (state.swapchainApi != API::Vulkan && !state.menuOverlayIsVulkan))
        return;
    if (potentiallyActive && !MenuOverlayBase::IsVisible())
        state.delayMenuRenderBy = 10;
    if (MenuOverlayBase::IsVisible())
    {
        options.mode = sl::DLSSGMode::eOff;
        options.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;
        ReflexHooks::setDlssgFrameCount(0);
    }
}

// SL INTERPOSER

void StreamlineHooks::unhookInterposer()
{
    LOG_FUNC();

    std::lock_guard<std::mutex> detoursLock(g_detoursTransactionMutex);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_slSetTag)
        DetourDetach(&(PVOID&) o_slSetTag, hkslSetTag);

    if (o_slSetTagForFrame)
        DetourDetach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

    if (o_slSetConstants)
        DetourDetach(&(PVOID&) o_slSetConstants, hkslSetConstants);

    if (o_slEvaluateFeature)
        DetourDetach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

    if (o_slInit)
        DetourDetach(&(PVOID&) o_slInit, hkslInit);

    if (o_slInit_sl1)
        DetourDetach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

    if (o_slSetTag_sl1)
        DetourDetach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

    if (o_slSetConstants_interposer_sl1)
        DetourDetach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

    if (o_slEvaluateFeature_sl1)
        DetourDetach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);

    // if (o_logCallback)
    //     DetourDetach(&(PVOID&) o_logCallback, streamlineLogCallback);
    // else if (o_logCallback_sl1)
    //     DetourDetach(&(PVOID&) o_logCallback_sl1, streamlineLogCallback);

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("DetourTransactionCommit error: {:X}", detourResult);
    }
    else
    {
        o_slInit = nullptr;
        o_slInit_sl1 = nullptr;
        o_slSetTag = nullptr;
        o_slSetTagForFrame = nullptr;
        o_slEvaluateFeature = nullptr;
        o_slSetConstants = nullptr;
        o_slSetTag_sl1 = nullptr;
        o_slSetConstants_interposer_sl1 = nullptr;
        o_slEvaluateFeature_sl1 = nullptr;
        o_logCallback = nullptr;
        o_logCallback_sl1 = nullptr;
    }
}

// Call it just after sl.interposer's load or if sl.interposer is already loaded
void StreamlineHooks::hookInterposer(HMODULE slInterposer)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_FUNC();

    if (!slInterposer)
    {
        LOG_WARN("Streamline module in NULL");
        return;
    }

    // Interposer needs this or it might end in an infinite loop calling itself
    static HMODULE last_slInterposer = nullptr;

    if (last_slInterposer == slInterposer)
        return;

    last_slInterposer = slInterposer;

    // Looks like when reading DLL version load methods are called
    // To prevent loops disabling checks for sl.interposer.dll
    auto owner = State::GetOwner();
    State::DisableChecks(owner, "sl.interposer");

    if (o_slSetTag || o_slInit || o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 ||
        o_slEvaluateFeature_sl1)
        unhookInterposer();

    {
        char dllPath[MAX_PATH];
        GetModuleFileNameA(slInterposer, dllPath, MAX_PATH);

        LOG_TRACE("slInterposer path: {}", dllPath);

        version_t sl_version;
        Util::GetFileVersion(string_to_wstring(dllPath), &sl_version);

        State::Instance().streamlineVersion.major = sl_version.major;
        State::Instance().streamlineVersion.minor = sl_version.minor;
        State::Instance().streamlineVersion.patch = sl_version.patch;

        LOG_INFO("Streamline version: {}.{}.{}", sl_version.major, sl_version.minor, sl_version.patch);

        if (sl_version.major >= 2)
        {
            o_slSetTag =
                reinterpret_cast<decltype(&slSetTag)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetTagForFrame = reinterpret_cast<decltype(&slSetTagForFrame)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTagForFrame"));
            o_slInit = reinterpret_cast<decltype(&slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slEvaluateFeature = reinterpret_cast<decltype(&slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));
            o_slAllocateResources = reinterpret_cast<decltype(&slAllocateResources)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slAllocateResources"));
            o_slSetConstants = reinterpret_cast<decltype(&slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slGetNativeInterface = reinterpret_cast<decltype(&slGetNativeInterface)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNativeInterface"));
            o_slSetD3DDevice = reinterpret_cast<decltype(&slSetD3DDevice)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetD3DDevice"));
            o_slGetNewFrameToken = reinterpret_cast<decltype(&slGetNewFrameToken)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetNewFrameToken")); // Not hooked

            // For making the game think DLSSG is loaded and supported
            // but making SL not actually load the plugin
            o_slIsFeatureSupported = reinterpret_cast<decltype(&slIsFeatureSupported)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slIsFeatureSupported"));
            o_slIsFeatureLoaded = reinterpret_cast<decltype(&slIsFeatureLoaded)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slIsFeatureLoaded"));
            o_slGetFeatureRequirements = reinterpret_cast<decltype(&slGetFeatureRequirements)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureRequirements"));
            o_slGetFeatureVersion = reinterpret_cast<decltype(&slGetFeatureVersion)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureVersion"));
            o_slGetFeatureFunction = reinterpret_cast<decltype(&slGetFeatureFunction)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slGetFeatureFunction"));

            if (o_slInit != nullptr)
            {
                LOG_TRACE("Hooking v2");
                std::lock_guard<std::mutex> detoursLock(g_detoursTransactionMutex);
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                DetourAttach(&(PVOID&) o_slInit, hkslInit);

                if (o_slEvaluateFeature != nullptr)
                    DetourAttach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);

                if (State::Instance().activeFgInput == FGInput::NvngxFG ||
                    State::Instance().activeFgInput == FGInput::DLSSG)
                {
                    if (o_slSetTag != nullptr)
                        DetourAttach(&(PVOID&) o_slSetTag, hkslSetTag);

                    if (o_slSetTagForFrame != nullptr)
                        DetourAttach(&(PVOID&) o_slSetTagForFrame, hkslSetTagForFrame);

                    if (o_slSetConstants != nullptr)
                        DetourAttach(&(PVOID&) o_slSetConstants, hkslSetConstants);
                }

                if (State::Instance().activeFgInput == FGInput::DLSSG)
                {
                    if (o_slIsFeatureSupported != nullptr)
                        DetourAttach(&(PVOID&) o_slIsFeatureSupported, hkslIsFeatureSupported);

                    if (o_slIsFeatureLoaded != nullptr)
                        DetourAttach(&(PVOID&) o_slIsFeatureLoaded, hkslIsFeatureLoaded);

                    if (o_slGetFeatureRequirements != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureRequirements, hkslGetFeatureRequirements);

                    if (o_slGetFeatureVersion != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureVersion, hkslGetFeatureVersion);

                    if (o_slGetFeatureFunction != nullptr)
                        DetourAttach(&(PVOID&) o_slGetFeatureFunction, hkslGetFeatureFunction);
                }

                // if (o_slAllocateResources != nullptr)
                //     DetourAttach(&(PVOID&) o_slAllocateResources, hkslAllocateResources);

                // if (o_slGetNativeInterface != nullptr)
                //     DetourAttach(&(PVOID&) o_slGetNativeInterface, hkslGetNativeInterface);

                // if (o_slSetD3DDevice != nullptr)
                //     DetourAttach(&(PVOID&) o_slSetD3DDevice, hkslSetD3DDevice);

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v2: {:X}", detourResult);
                    o_slSetTag = nullptr;
                    o_slSetTagForFrame = nullptr;
                    o_slInit = nullptr;
                    o_slEvaluateFeature = nullptr;
                    o_slAllocateResources = nullptr;
                    o_slSetConstants = nullptr;
                    o_slGetNativeInterface = nullptr;
                    o_slSetD3DDevice = nullptr;
                    o_slIsFeatureSupported = nullptr;
                    o_slIsFeatureLoaded = nullptr;
                    o_slGetFeatureRequirements = nullptr;
                    o_slGetFeatureVersion = nullptr;
                    o_slGetFeatureFunction = nullptr;
                }
            }
        }
        else if (sl_version.major == 1)
        {
            o_slInit_sl1 =
                reinterpret_cast<decltype(&sl1::slInit)>(KernelBaseProxy::GetProcAddress_()(slInterposer, "slInit"));
            o_slSetTag_sl1 = reinterpret_cast<decltype(&sl1::slSetTag)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetTag"));
            o_slSetConstants_interposer_sl1 = reinterpret_cast<decltype(&sl1::slSetConstants)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slSetConstants"));
            o_slEvaluateFeature_sl1 = reinterpret_cast<decltype(&sl1::slEvaluateFeature)>(
                KernelBaseProxy::GetProcAddress_()(slInterposer, "slEvaluateFeature"));

            LOG_INFO("SL1 exports - slInit: {}, slSetTag: {}, slSetConstants: {}, slEvaluateFeature: {}",
                     o_slInit_sl1 != nullptr, o_slSetTag_sl1 != nullptr, o_slSetConstants_interposer_sl1 != nullptr,
                     o_slEvaluateFeature_sl1 != nullptr);

            if (o_slInit_sl1 || o_slSetTag_sl1 || o_slSetConstants_interposer_sl1 || o_slEvaluateFeature_sl1)
            {
                LOG_TRACE("Hooking v1");
                std::lock_guard<std::mutex> detoursLock(g_detoursTransactionMutex);
                DetourTransactionBegin();
                DetourUpdateThread(GetCurrentThread());

                if (o_slInit_sl1)
                    DetourAttach(&(PVOID&) o_slInit_sl1, hkslInit_sl1);

                if (IsSL1AndFGActive())
                {
                    if (o_slSetTag_sl1)
                        DetourAttach(&(PVOID&) o_slSetTag_sl1, hkslSetTag_sl1);

                    if (o_slSetConstants_interposer_sl1)
                        DetourAttach(&(PVOID&) o_slSetConstants_interposer_sl1, hkslSetConstants_sl1);

                    if (o_slEvaluateFeature_sl1)
                        DetourAttach(&(PVOID&) o_slEvaluateFeature_sl1, hkslEvaluateFeature_sl1);
                }

                auto detourResult = DetourTransactionCommit();
                if (detourResult != NO_ERROR)
                {
                    LOG_ERROR("Failed to hook sl.interposer v1: {:X}", detourResult);
                    o_slInit_sl1 = nullptr;
                    o_slSetTag_sl1 = nullptr;
                    o_slSetConstants_interposer_sl1 = nullptr;
                    o_slEvaluateFeature_sl1 = nullptr;
                }
            }
        }
    }

    State::EnableChecks(owner);
}

// Streamline plugins (sl.dlss, sl.dlss_g, sl.reflex, sl.pcl, sl.common): one detour on each plugin's
// slGetPluginFunction. Every load of a plugin comes through here, also a load of one already in memory (Streamline and
// games load them more than once). Taking the detour off and putting it back each time removed it while game threads
// called through it, or stacked two detours on one function that then called each other: the intermittent crash or
// freeze at startup. So the detour stays while it is in place, and only a fresh copy of a plugin (loaded again after an
// unload, at the same or another address) is hooked again.
//
// What the function starts with tells the cases apart, so both the bytes Detours wrote (our jump) and the bytes it
// replaced (the plugin's own) are kept:
// - our jump: hooked, nothing to do;
// - the plugin's own bytes: a fresh copy (or our detour taken off): hook it;
// - anything else: another tool hooked it on top of ours (an overlay, a mod). Hooking again would stack a second detour
//   behind theirs, which calls back into ours forever; taking ours off is not possible either. Left alone, and said
//   once.
//
// Each hook calls one original, so only one copy of a plugin can carry our detour. When another copy is loaded (the
// game's and the driver's OTA sl.dlss_g, from two folders), the earlier copy's detour is taken off by writing its own
// bytes back in one aligned 8-byte store, and its trampoline is kept: a game thread already inside the hook still runs
// the earlier copy's prologue and returns into its code, where a Detours detach would have freed the trampoline under
// it. (A few dozen bytes per copy change, a handful of times per session.) If the earlier copy cannot be taken off, the
// new copy is not hooked: the earlier one keeps working, rather than both calling one copy's code.
struct PluginHook
{
    const char* logName;   // "Hooking slGetPluginFunction in <logName>"
    const char* errorName; // "Failed to hook <errorName>"
    HMODULE module = nullptr;
    void* target =
        nullptr; // where Detours wrote our jump (the plugin's slGetPluginFunction, after any jump it follows)
    // Its first bytes as Detours left them (our jump, then int3 to the end of the last instruction it moved) and as
    // they were before. The first 8 tell the cases apart; all of them are put back when our detour is taken off.
    uint8_t patch[32] {};
    uint8_t pristine[32] {};
    bool saidForeign = false;
};

static PluginHook g_dlssPlugin { "sl.dlss", "DLSS" };
static PluginHook g_dlssgPlugin { "sl.dlssg", "DLSSG" };
static PluginHook g_localDlssgPlugin { "local sl.dlssg", "local DLSSG" };
static PluginHook g_reflexPlugin { "sl.reflex", "Reflex" };
static PluginHook g_pclPlugin { "sl.pcl", "PCL" };
static PluginHook g_commonPlugin { "sl.common", "Common" };

// Held around each plugin's record and its Detours transaction (taken before g_detoursTransactionMutex, never after).
// Nothing under it loads a library, waits for the loader or frees a module: a module pinned for a check (PluginPin) is
// released after it.
static std::mutex g_pluginHookMutex;

enum class PluginState
{
    Gone,     // no record, or its module is not loaded any more
    Ours,     // our jump is in place
    Pristine, // the plugin's own bytes: a fresh copy at the same address, or our detour taken off
    Foreign   // something else is written there
};

// A module held loaded while its bytes are read or written, released once g_pluginHookMutex is let go (FreeLibrary may
// unload it, and unloading takes the loader lock)
struct PluginPin
{
    HMODULE module = nullptr;
    ~PluginPin()
    {
        if (module != nullptr)
            FreeLibrary(module);
    }
};

// The record's function as it stands now (caller holds g_pluginHookMutex). `pin` keeps its module loaded until the
// caller has released the lock.
static PluginState StateOf(const PluginHook& plugin, PluginPin& pin)
{
    if (plugin.target == nullptr)
        return PluginState::Gone;

    HMODULE owner = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, static_cast<LPCWSTR>(plugin.target), &owner))
        return PluginState::Gone;

    pin.module = owner;

    if (owner != plugin.module)
        return PluginState::Gone;

    if (std::memcmp(plugin.target, plugin.patch, 8) == 0)
        return PluginState::Ours;

    if (std::memcmp(plugin.target, plugin.pristine, 8) == 0)
        return PluginState::Pristine;

    return PluginState::Foreign;
}

// Takes our detour off a copy still carrying it (caller holds g_pluginHookMutex and a pin on its module), keeping its
// trampoline for any thread already inside the hook. The bytes past the first 8 go back first (behind our jump, nothing
// runs them), then the first 8 in one aligned 8-byte store, so a thread entering the function sees either our jump or
// its own code, never half of each. False when the first bytes are not ours any more or the function is not 8-byte
// aligned; nothing is changed then.
static bool TakeOffPlugin(PluginHook& plugin)
{
    if ((reinterpret_cast<uintptr_t>(plugin.target) & 7) != 0)
        return false;

    auto* code = static_cast<uint8_t*>(plugin.target);
    LONG64 ours = 0, own = 0;
    std::memcpy(&ours, plugin.patch, sizeof(ours));
    std::memcpy(&own, plugin.pristine, sizeof(own));

    DWORD protection = 0;

    if (!VirtualProtect(code, sizeof(plugin.patch), PAGE_EXECUTE_READWRITE, &protection))
        return false;

    bool done = false;

    if (std::memcmp(code, &ours, sizeof(ours)) == 0)
    {
        for (size_t i = sizeof(ours); i < sizeof(plugin.patch); ++i)
        {
            if (code[i] == plugin.patch[i])
                code[i] = plugin.pristine[i];
        }

        done = InterlockedCompareExchange64(reinterpret_cast<LONG64*>(code), own, ours) == ours;
    }

    VirtualProtect(code, sizeof(plugin.patch), protection, &protection);
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(plugin.patch));
    return done;
}

// The fallback for a function TakeOffPlugin cannot handle (not 8-byte aligned): a Detours detach, as before this record
// existed. It frees the trampoline, which a thread already inside the hook may still be about to call.
template <typename T> static bool DetachPlugin(PluginHook& plugin, T& original, void* hook)
{
    PVOID trampoline = reinterpret_cast<PVOID>(LoadOriginal(original));

    std::lock_guard<std::mutex> detoursLock(g_detoursTransactionMutex);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&trampoline, hook);

    if (const auto detourResult = DetourTransactionCommit(); detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook {}: {:X}", plugin.errorName, detourResult);
        return false;
    }

    return true;
}

// Hooks `module`'s slGetPluginFunction unless our detour is already in place on it (see PluginHook). `onNewCopy` runs
// when the record of an earlier copy is let go.
template <typename T>
static void HookPlugin(PluginHook& plugin, HMODULE module, T& original, void* hook, void (*onNewCopy)() = nullptr)
{
    // Outside our lock: it may wait for the loader
    auto function = reinterpret_cast<void*>(KernelBaseProxy::GetProcAddress_()(module, "slGetPluginFunction"));

    if (function == nullptr)
    {
        LOG_WARN("No slGetPluginFunction in {}", plugin.logName);
        return;
    }

    PluginPin pin; // released after the lock below
    std::lock_guard<std::mutex> lock(g_pluginHookMutex);
    const PluginState state = StateOf(plugin, pin);

    if (plugin.module == module)
    {
        if (state == PluginState::Ours)
        {
            LOG_TRACE("slGetPluginFunction in {} is already hooked", plugin.logName);
            return;
        }

        if (state == PluginState::Foreign)
        {
            if (!plugin.saidForeign)
            {
                plugin.saidForeign = true;
                LOG_WARN("slGetPluginFunction in {} was hooked by something else on top of ours; leaving it as it is",
                         plugin.logName);
            }

            return;
        }
    }
    else if (state == PluginState::Ours && !TakeOffPlugin(plugin) && !DetachPlugin(plugin, original, hook))
    {
        LOG_ERROR("Failed to hook {}: the earlier copy's hook could not be taken off, so it stays in use",
                  plugin.errorName);
        return;
    }
    else if (state == PluginState::Foreign)
    {
        LOG_ERROR("Failed to hook {}: the earlier copy is hooked by something else on top of ours, so it stays in use",
                  plugin.errorName);
        return;
    }

    // The earlier record (if any) is let go: its copy is gone, was loaded again, or no longer carries our detour
    if (plugin.target != nullptr && onNewCopy != nullptr)
        onNewCopy();

    plugin.module = nullptr;
    plugin.target = nullptr;
    plugin.saidForeign = false;

    LOG_TRACE("Hooking slGetPluginFunction in {}", plugin.logName);

    std::lock_guard<std::mutex> detoursLock(g_detoursTransactionMutex);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    // The trampoline is built here, before the jump is written at the commit: published first, so no thread ever finds
    // the hook live with no original (or the earlier copy's) to call
    PVOID pointer = function;
    PDETOUR_TRAMPOLINE trampoline = nullptr;
    PVOID target = nullptr;

    if (const auto attachResult = DetourAttachEx(&pointer, hook, &trampoline, &target, nullptr);
        attachResult != NO_ERROR || target == nullptr)
    {
        DetourTransactionAbort();
        LOG_ERROR("Failed to hook {}: {:X}", plugin.errorName, attachResult);
        return;
    }

    // Read before the commit writes the jump: the plugin's own bytes
    uint8_t pristine[sizeof(plugin.pristine)] {};
    std::memcpy(pristine, target, sizeof(pristine));

    const T previous = LoadOriginal(original);
    StoreOriginal(original, reinterpret_cast<T>(trampoline));

    if (const auto detourResult = DetourTransactionCommit(); detourResult != NO_ERROR)
    {
        StoreOriginal(original, previous);
        LOG_ERROR("Failed to hook {}: {:X}", plugin.errorName, detourResult);
        return;
    }

    plugin.module = module;
    plugin.target = target;
    std::memcpy(plugin.pristine, pristine, sizeof(pristine));
    std::memcpy(plugin.patch, target, sizeof(plugin.patch));

    // Detours writes a 5-byte jump; anything else here means it patched somewhere this record cannot follow
    if (static_cast<const uint8_t*>(target)[0] != 0xE9)
        LOG_WARN("slGetPluginFunction in {}: unexpected hook bytes; reloads of it may not be told apart",
                 plugin.logName);
}

// Our detour is in place on the plugin's current copy
static bool PluginHooked(PluginHook& plugin)
{
    PluginPin pin;
    std::lock_guard<std::mutex> lock(g_pluginHookMutex);
    return StateOf(plugin, pin) == PluginState::Ours;
}

// SL DLSS

void StreamlineHooks::hookDlss(HMODULE slDlss)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_FUNC();

    if (!slDlss)
    {
        LOG_WARN("Dlss module in NULL");
        return;
    }

    HookPlugin(g_dlssPlugin, slDlss, o_dlss_slGetPluginFunction, reinterpret_cast<void*>(hkdlss_slGetPluginFunction));
}

// SL DLSSG

void StreamlineHooks::hookDlssg(HMODULE slDlssg)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_FUNC();

    if (!slDlssg)
    {
        LOG_WARN("Dlssg module in NULL");
        return;
    }

    HookPlugin(g_dlssgPlugin, slDlssg, o_dlssg_slGetPluginFunction,
               reinterpret_cast<void*>(hkdlssg_slGetPluginFunction));
}

// Local SL DLSSG

void StreamlineHooks::hookLocalDlssg(HMODULE slDlssg)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_FUNC();

    if (!slDlssg)
    {
        LOG_WARN("Dlssg module in NULL");
        return;
    }

    HookPlugin(g_localDlssgPlugin, slDlssg, o_local_dlssg_slGetPluginFunction,
               reinterpret_cast<void*>(hklocal_dlssg_slGetPluginFunction));
}

// SL REFLEX

void StreamlineHooks::hookReflex(HMODULE slReflex)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_FUNC();

    if (!slReflex)
    {
        LOG_WARN("Reflex module in NULL");
        return;
    }

    HookPlugin(g_reflexPlugin, slReflex, o_reflex_slGetPluginFunction,
               reinterpret_cast<void*>(hkreflex_slGetPluginFunction));
}

// SL PCL

void StreamlineHooks::hookPcl(HMODULE slPcl)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_FUNC();

    if (!slPcl)
    {
        LOG_WARN("Pcl module in NULL");
        return;
    }

    HookPlugin(g_pclPlugin, slPcl, o_pcl_slGetPluginFunction, reinterpret_cast<void*>(hkpcl_slGetPluginFunction));
}

// SL COMMON

void StreamlineHooks::hookCommon(HMODULE slCommon)
{
    if (State::Instance().externalFrameGeneration)
        return;
    LOG_FUNC();

    if (!slCommon)
    {
        LOG_WARN("Common module in NULL");
        return;
    }

    // Read again on the next plugin load (hookSystemCaps), as on every load of sl.common before: a game that starts
    // Streamline again while sl.common stays loaded may hand out new caps
    systemCaps = nullptr;
    systemCapsSl15 = nullptr;

    HookPlugin(g_commonPlugin, slCommon, o_common_slGetPluginFunction,
               reinterpret_cast<void*>(hkcommon_slGetPluginFunction));
}

bool StreamlineHooks::isInterposerHooked() { return o_slInit != nullptr || o_slInit_sl1 != nullptr; }

// Hooked: our detour is in place on the plugin's current copy (a copy unloaded since is not; CheckModulesInMemory then
// hooks the one loaded now)
bool StreamlineHooks::isDlssHooked() { return PluginHooked(g_dlssPlugin); }

bool StreamlineHooks::isDlssgHooked() { return PluginHooked(g_dlssgPlugin); }

bool StreamlineHooks::isLocalDlssgHooked() { return PluginHooked(g_localDlssgPlugin); }

bool StreamlineHooks::isCommonHooked() { return PluginHooked(g_commonPlugin); }

bool StreamlineHooks::isPclHooked() { return PluginHooked(g_pclPlugin); }

bool StreamlineHooks::isReflexHooked() { return PluginHooked(g_reflexPlugin); }
