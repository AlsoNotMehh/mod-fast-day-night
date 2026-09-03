#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "Weather.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
constexpr float NormalGameSpeed = 0.01666667f;
constexpr float DefaultVisualSpeed = 3.0f;
constexpr char EnvironmentTable[] = "Custom`.`fast_environment_state";
constexpr AccountTypes EnvironmentCommandSecurity = SEC_ADMINISTRATOR;

enum class EnvironmentScope : uint8
{
    Global = 0,
    Zone = 1
};

struct FastEnvironmentConfig
{
    bool Enabled = true;
    float DefaultSpeed = DefaultVisualSpeed;
    float MaximumSpeed = 288.0f;
    int32 OffsetHours = 0;
    uint32 SyncIntervalSeconds = 300;
    uint32 DefaultLightFadeMilliseconds = 3000;
};

struct ClockState
{
    time_t RealTimeAnchor = 0;
    double VisualTimeAnchor = 0.0;
    float Speed = DefaultVisualSpeed;
};

struct WeatherOverride
{
    WeatherState State = WEATHER_STATE_FINE;
    float Intensity = 0.0f;
    bool Abrupt = false;
};

struct LightOverride
{
    uint32 LightId = 0;
    uint32 FadeMilliseconds = 3000;
};

struct EnvironmentState
{
    std::optional<ClockState> Clock;
    std::optional<WeatherOverride> Weather;
    std::optional<LightOverride> Light;
    std::optional<uint32> Music;

    [[nodiscard]] bool Empty() const
    {
        return !Clock && !Weather && !Light && !Music;
    }
};

struct MapZoneKey
{
    uint32 MapId = 0;
    uint32 InstanceId = 0;
    uint32 ZoneId = 0;

    bool operator==(MapZoneKey const& right) const
    {
        return MapId == right.MapId &&
            InstanceId == right.InstanceId &&
            ZoneId == right.ZoneId;
    }
};

struct MapZoneKeyHash
{
    std::size_t operator()(MapZoneKey const& key) const
    {
        std::size_t value = key.MapId;
        value = value * 1315423911u + key.InstanceId;
        value = value * 1315423911u + key.ZoneId;
        return value;
    }
};

FastEnvironmentConfig Config;
EnvironmentState GlobalState;
std::unordered_map<uint32, EnvironmentState> ZoneStates;
std::unordered_set<MapZoneKey, MapZoneKeyHash> AppliedWeather;
std::unordered_set<MapZoneKey, MapZoneKeyHash> AppliedLight;
std::unordered_set<MapZoneKey, MapZoneKeyHash> AppliedMusic;
uint32 SyncTimer = 0;
bool RuntimeInitialized = false;

bool GetLocalTime(time_t value, std::tm& localTime)
{
#if defined(_WIN32)
    return localtime_s(&localTime, &value) == 0;
#else
    return localtime_r(&value, &localTime) != nullptr;
#endif
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

float ClampSpeed(float speed)
{
    if (!std::isfinite(speed))
        return Config.DefaultSpeed;

    return std::clamp(speed, 0.1f, Config.MaximumSpeed);
}

float ClampIntensity(float intensity)
{
    if (!std::isfinite(intensity))
        return 0.65f;

    return std::clamp(intensity, 0.0f, 0.9999f);
}

time_t GetVisualTime(ClockState const& clock)
{
    time_t const now = std::time(nullptr);
    double const elapsedRealSeconds =
        std::difftime(now, clock.RealTimeAnchor);
    return static_cast<time_t>(
        clock.VisualTimeAnchor + elapsedRealSeconds * clock.Speed);
}

ClockState MakeDefaultClock()
{
    time_t const now = std::time(nullptr);
    ClockState clock;
    clock.RealTimeAnchor = now;
    clock.VisualTimeAnchor =
        double(now) + double(Config.OffsetHours) * HOUR;
    clock.Speed = Config.DefaultSpeed;
    return clock;
}

void ReanchorClock(ClockState& clock, time_t visualTime, float speed)
{
    clock.RealTimeAnchor = std::time(nullptr);
    clock.VisualTimeAnchor = double(visualTime);
    clock.Speed = ClampSpeed(speed);
}

ClockState const& GetGlobalClock()
{
    if (!GlobalState.Clock)
        GlobalState.Clock = MakeDefaultClock();

    return *GlobalState.Clock;
}

ClockState const& GetEffectiveClock(uint32 zoneId)
{
    auto const zone = ZoneStates.find(zoneId);
    if (zone != ZoneStates.end() && zone->second.Clock)
        return *zone->second.Clock;

    return GetGlobalClock();
}

template <typename Member>
auto GetEffectiveValue(uint32 zoneId, Member member)
    -> decltype(GlobalState.*member)
{
    auto const zone = ZoneStates.find(zoneId);
    if (zone != ZoneStates.end() && zone->second.*member)
        return zone->second.*member;

    return GlobalState.*member;
}

void SendConfiguredTime(Player* player)
{
    if (!player || !player->GetSession())
        return;

    WorldPacket packet(SMSG_LOGIN_SETTIMESPEED, 12);
    if (Config.Enabled)
    {
        ClockState const& clock = GetEffectiveClock(player->GetZoneId());
        packet.AppendPackedTime(GetVisualTime(clock));
        packet << float(NormalGameSpeed * clock.Speed);
    }
    else
    {
        packet.AppendPackedTime(GameTime::GetGameTime().count());
        packet << float(NormalGameSpeed);
    }

    packet << uint32(0);
    player->SendDirectMessage(&packet);
}

MapZoneKey MakeMapZoneKey(Map const* map, uint32 zoneId)
{
    return {
        map ? map->GetId() : 0,
        map ? map->GetInstanceId() : 0,
        zoneId
    };
}

void ApplyPersistentEnvironment(Map* map, uint32 zoneId)
{
    if (!map || !zoneId)
        return;

    MapZoneKey const key = MakeMapZoneKey(map, zoneId);

    std::optional<WeatherOverride> weather =
        GetEffectiveValue(zoneId, &EnvironmentState::Weather);
    if (Config.Enabled && weather)
    {
        map->SetZoneWeather(
            zoneId, weather->State, weather->Intensity, weather->Abrupt);
        AppliedWeather.insert(key);
    }
    else if (AppliedWeather.erase(key))
        map->SetZoneWeather(zoneId, WEATHER_STATE_FINE, 0.0f);

    std::optional<LightOverride> light =
        GetEffectiveValue(zoneId, &EnvironmentState::Light);
    if (Config.Enabled && light)
    {
        map->SetZoneOverrideLight(
            zoneId,
            light->LightId,
            Milliseconds(light->FadeMilliseconds));
        AppliedLight.insert(key);
    }
    else if (AppliedLight.erase(key))
    {
        map->SetZoneOverrideLight(
            zoneId, 0, Milliseconds(Config.DefaultLightFadeMilliseconds));
    }

    std::optional<uint32> music =
        GetEffectiveValue(zoneId, &EnvironmentState::Music);
    if (Config.Enabled && music)
    {
        map->SetZoneMusic(zoneId, *music);
        AppliedMusic.insert(key);
    }
    else if (AppliedMusic.erase(key))
        map->SetZoneMusic(zoneId, 0);
}

void ApplyEnvironment(Player* player)
{
    if (!player || !player->IsInWorld())
        return;

    SendConfiguredTime(player);
    ApplyPersistentEnvironment(player->FindMap(), player->GetZoneId());
}

void SyncAllPlayerTimes()
{
    sWorldSessionMgr->DoForAllOnlinePlayers(
        [](Player* player)
        {
            SendConfiguredTime(player);
        });
}

void RefreshAppliedEnvironment()
{
    auto refreshTracked =
        [](std::unordered_set<MapZoneKey, MapZoneKeyHash> const& tracked)
        {
            std::vector<MapZoneKey> keys(tracked.begin(), tracked.end());
            for (MapZoneKey const& key : keys)
            {
                sMapMgr->DoForAllMapsWithMapId(
                    key.MapId,
                    [&key](Map* map)
                    {
                        if (map->GetInstanceId() == key.InstanceId)
                            ApplyPersistentEnvironment(map, key.ZoneId);
                    });
            }
        };

    refreshTracked(AppliedWeather);
    refreshTracked(AppliedLight);
    refreshTracked(AppliedMusic);

    sWorldSessionMgr->DoForAllOnlinePlayers(
        [](Player* player)
        {
            ApplyEnvironment(player);
        });
}

void LoadConfig()
{
    Config.Enabled =
        sConfigMgr->GetOption<bool>("FastDayNight.Enable", true);
    Config.MaximumSpeed = std::clamp(
        sConfigMgr->GetOption<float>(
            "FastDayNight.MaximumSpeed", 288.0f),
        1.0f, 1440.0f);
    Config.DefaultSpeed = ClampSpeed(
        sConfigMgr->GetOption<float>(
            "FastDayNight.Speed", DefaultVisualSpeed));
    Config.OffsetHours = std::clamp(
        sConfigMgr->GetOption<int32>(
            "FastDayNight.OffsetHours", 0),
        int32(-23), int32(23));
    Config.SyncIntervalSeconds = std::clamp(
        sConfigMgr->GetOption<uint32>(
            "FastDayNight.SyncIntervalSeconds", 300),
        uint32(30), uint32(3600));
    Config.DefaultLightFadeMilliseconds = std::clamp(
        sConfigMgr->GetOption<uint32>(
            "FastDayNight.DefaultLightFadeMilliseconds", 3000),
        uint32(0), uint32(60000));

    if (!RuntimeInitialized)
        GlobalState.Clock = MakeDefaultClock();
}

void EnsureEnvironmentTable()
{
    WorldDatabase.Execute(
        "CREATE DATABASE IF NOT EXISTS `Custom` "
        "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci");
    WorldDatabase.Execute(
        "CREATE TABLE IF NOT EXISTS `Custom`.`fast_environment_state` ("
        "`scope_type` TINYINT UNSIGNED NOT NULL,"
        "`scope_id` INT UNSIGNED NOT NULL,"
        "`clock_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`visual_anchor` BIGINT NOT NULL DEFAULT 0,"
        "`real_anchor` BIGINT NOT NULL DEFAULT 0,"
        "`speed` DOUBLE NOT NULL DEFAULT 1,"
        "`weather_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`weather_state` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`weather_intensity` FLOAT NOT NULL DEFAULT 0,"
        "`weather_abrupt` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`light_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`light_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "`light_fade_ms` INT UNSIGNED NOT NULL DEFAULT 3000,"
        "`music_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        "`music_id` INT UNSIGNED NOT NULL DEFAULT 0,"
        "PRIMARY KEY (`scope_type`, `scope_id`)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
}

void SaveState(
    EnvironmentScope scope, uint32 scopeId,
    EnvironmentState const& state)
{
    if (scope == EnvironmentScope::Zone && state.Empty())
    {
        WorldDatabase.Execute(
            "DELETE FROM `{}` WHERE `scope_type` = {} AND `scope_id` = {}",
            EnvironmentTable,
            static_cast<uint32>(scope),
            scopeId);
        return;
    }

    ClockState const emptyClock = {};
    WeatherOverride const emptyWeather = {};
    LightOverride const emptyLight = {};
    ClockState const& clock = state.Clock ? *state.Clock : emptyClock;
    WeatherOverride const& weather =
        state.Weather ? *state.Weather : emptyWeather;
    LightOverride const& light =
        state.Light ? *state.Light : emptyLight;

    WorldDatabase.Execute(
        "REPLACE INTO `{}` ("
        "`scope_type`,`scope_id`,`clock_enabled`,`visual_anchor`,"
        "`real_anchor`,`speed`,`weather_enabled`,`weather_state`,"
        "`weather_intensity`,`weather_abrupt`,`light_enabled`,`light_id`,"
        "`light_fade_ms`,`music_enabled`,`music_id`) VALUES "
        "({},{},{},{},{},{},{},{},{},{},{},{},{},{},{})",
        EnvironmentTable,
        static_cast<uint32>(scope),
        scopeId,
        state.Clock ? 1 : 0,
        static_cast<int64>(clock.VisualTimeAnchor),
        static_cast<int64>(clock.RealTimeAnchor),
        clock.Speed,
        state.Weather ? 1 : 0,
        static_cast<uint32>(weather.State),
        weather.Intensity,
        weather.Abrupt ? 1 : 0,
        state.Light ? 1 : 0,
        light.LightId,
        light.FadeMilliseconds,
        state.Music ? 1 : 0,
        state.Music.value_or(0));
}

bool IsValidWeatherState(uint32 value)
{
    switch (WeatherState(value))
    {
        case WEATHER_STATE_FINE:
        case WEATHER_STATE_FOG:
        case WEATHER_STATE_LIGHT_RAIN:
        case WEATHER_STATE_MEDIUM_RAIN:
        case WEATHER_STATE_HEAVY_RAIN:
        case WEATHER_STATE_LIGHT_SNOW:
        case WEATHER_STATE_MEDIUM_SNOW:
        case WEATHER_STATE_HEAVY_SNOW:
        case WEATHER_STATE_LIGHT_SANDSTORM:
        case WEATHER_STATE_MEDIUM_SANDSTORM:
        case WEATHER_STATE_HEAVY_SANDSTORM:
        case WEATHER_STATE_THUNDERS:
        case WEATHER_STATE_BLACKRAIN:
        case WEATHER_STATE_BLACKSNOW:
            return true;
        default:
            return false;
    }
}

void LoadPersistentState()
{
    EnsureEnvironmentTable();
    ZoneStates.clear();

    QueryResult result = WorldDatabase.Query(
        "SELECT `scope_type`,`scope_id`,`clock_enabled`,`visual_anchor`,"
        "`real_anchor`,`speed`,`weather_enabled`,`weather_state`,"
        "`weather_intensity`,`weather_abrupt`,`light_enabled`,`light_id`,"
        "`light_fade_ms`,`music_enabled`,`music_id` FROM `{}`",
        EnvironmentTable);
    if (!result)
    {
        RuntimeInitialized = true;
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        EnvironmentScope scope =
            EnvironmentScope(fields[0].Get<uint8>());
        uint32 scopeId = fields[1].Get<uint32>();
        if (scope != EnvironmentScope::Global &&
            scope != EnvironmentScope::Zone)
            continue;

        EnvironmentState loaded;
        if (fields[2].Get<bool>())
        {
            ClockState clock;
            clock.VisualTimeAnchor =
                double(fields[3].Get<int64>());
            clock.RealTimeAnchor =
                time_t(fields[4].Get<int64>());
            clock.Speed = ClampSpeed(fields[5].Get<float>());
            if (clock.RealTimeAnchor > 0 &&
                clock.VisualTimeAnchor > 0.0)
                loaded.Clock = clock;
        }

        if (fields[6].Get<bool>())
        {
            uint32 state = fields[7].Get<uint32>();
            if (IsValidWeatherState(state))
            {
                loaded.Weather = WeatherOverride{
                    WeatherState(state),
                    ClampIntensity(fields[8].Get<float>()),
                    fields[9].Get<bool>()
                };
            }
        }

        if (fields[10].Get<bool>())
        {
            loaded.Light = LightOverride{
                fields[11].Get<uint32>(),
                std::min<uint32>(
                    fields[12].Get<uint32>(), 60000)
            };
        }

        if (fields[13].Get<bool>())
            loaded.Music = fields[14].Get<uint32>();

        if (scope == EnvironmentScope::Global && scopeId == 0)
        {
            if (loaded.Clock)
                GlobalState.Clock = loaded.Clock;
            GlobalState.Weather = loaded.Weather;
            GlobalState.Light = loaded.Light;
            GlobalState.Music = loaded.Music;
        }
        else if (scope == EnvironmentScope::Zone && scopeId)
            ZoneStates[scopeId] = std::move(loaded);
    } while (result->NextRow());

    RuntimeInitialized = true;
}

bool ParseClockTime(std::string value, uint8& hour, uint8& minute)
{
    value = ToLower(value);
    if (value == "dawn")
        value = "06:00";
    else if (value == "noon")
        value = "12:00";
    else if (value == "dusk")
        value = "18:00";
    else if (value == "night")
        value = "22:00";
    else if (value == "midnight")
        value = "00:00";

    int parsedHour = -1;
    int parsedMinute = 0;
    char trailing = '\0';
    int matched = std::sscanf(
        value.c_str(), "%d:%d%c",
        &parsedHour, &parsedMinute, &trailing);
    if (matched == 1)
        parsedMinute = 0;
    else if (matched != 2)
        return false;

    if (parsedHour < 0 || parsedHour > 23 ||
        parsedMinute < 0 || parsedMinute > 59)
        return false;

    hour = static_cast<uint8>(parsedHour);
    minute = static_cast<uint8>(parsedMinute);
    return true;
}

time_t SetVisualClockTime(time_t current, uint8 hour, uint8 minute)
{
    std::tm localTime = {};
    if (!GetLocalTime(current, localTime))
        return current;

    localTime.tm_hour = hour;
    localTime.tm_min = minute;
    localTime.tm_sec = 0;
    localTime.tm_isdst = -1;
    return std::mktime(&localTime);
}

std::string FormatClock(ClockState const& clock)
{
    time_t visual = GetVisualTime(clock);
    std::tm localTime = {};
    if (!GetLocalTime(visual, localTime))
        return "unknown";

    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
    return buffer;
}

WeatherState GetPrecipitationState(
    WeatherState light, WeatherState medium, WeatherState heavy,
    float intensity)
{
    if (intensity < 0.40f)
        return light;
    if (intensity < 0.70f)
        return medium;
    return heavy;
}

std::optional<WeatherState> ParseWeatherState(
    std::string value, float intensity)
{
    value = ToLower(value);
    if (value == "fine" || value == "clear" || value == "normal")
        return WEATHER_STATE_FINE;
    if (value == "fog")
        return WEATHER_STATE_FOG;
    if (value == "rain")
    {
        return GetPrecipitationState(
            WEATHER_STATE_LIGHT_RAIN,
            WEATHER_STATE_MEDIUM_RAIN,
            WEATHER_STATE_HEAVY_RAIN,
            intensity);
    }
    if (value == "snow")
    {
        return GetPrecipitationState(
            WEATHER_STATE_LIGHT_SNOW,
            WEATHER_STATE_MEDIUM_SNOW,
            WEATHER_STATE_HEAVY_SNOW,
            intensity);
    }
    if (value == "storm" || value == "sandstorm")
    {
        return GetPrecipitationState(
            WEATHER_STATE_LIGHT_SANDSTORM,
            WEATHER_STATE_MEDIUM_SANDSTORM,
            WEATHER_STATE_HEAVY_SANDSTORM,
            intensity);
    }
    if (value == "thunder" || value == "thunders")
        return WEATHER_STATE_THUNDERS;
    if (value == "blackrain")
        return WEATHER_STATE_BLACKRAIN;
    if (value == "blacksnow")
        return WEATHER_STATE_BLACKSNOW;

    return std::nullopt;
}

char const* GetWeatherName(WeatherState state)
{
    switch (state)
    {
        case WEATHER_STATE_FINE:
            return "clear";
        case WEATHER_STATE_FOG:
            return "fog";
        case WEATHER_STATE_LIGHT_RAIN:
            return "light rain";
        case WEATHER_STATE_MEDIUM_RAIN:
            return "medium rain";
        case WEATHER_STATE_HEAVY_RAIN:
            return "heavy rain";
        case WEATHER_STATE_LIGHT_SNOW:
            return "light snow";
        case WEATHER_STATE_MEDIUM_SNOW:
            return "medium snow";
        case WEATHER_STATE_HEAVY_SNOW:
            return "heavy snow";
        case WEATHER_STATE_LIGHT_SANDSTORM:
            return "light sandstorm";
        case WEATHER_STATE_MEDIUM_SANDSTORM:
            return "medium sandstorm";
        case WEATHER_STATE_HEAVY_SANDSTORM:
            return "heavy sandstorm";
        case WEATHER_STATE_THUNDERS:
            return "thunder";
        case WEATHER_STATE_BLACKRAIN:
            return "black rain";
        case WEATHER_STATE_BLACKSNOW:
            return "black snow";
        default:
            return "unknown";
    }
}

EnvironmentState& GetZoneState(uint32 zoneId)
{
    return ZoneStates[zoneId];
}

bool RequirePlayerZone(
    ChatHandler* handler, Player*& player, uint32& zoneId)
{
    player = handler->GetPlayer();
    if (!player || !player->IsInWorld())
    {
        handler->SendSysMessage(
            "This zone command must be executed in game.");
        return false;
    }

    zoneId = player->GetZoneId();
    if (!zoneId)
    {
        handler->SendSysMessage("The current zone could not be identified.");
        return false;
    }

    return true;
}

void SaveGlobalState()
{
    SaveState(EnvironmentScope::Global, 0, GlobalState);
}

void SaveZoneState(uint32 zoneId)
{
    auto zone = ZoneStates.find(zoneId);
    if (zone == ZoneStates.end())
        return;

    SaveState(EnvironmentScope::Zone, zoneId, zone->second);
    if (zone->second.Empty())
        ZoneStates.erase(zone);
}

void PrintEnvironmentStatus(
    ChatHandler* handler, uint32 zoneId, bool includeZone)
{
    ClockState const& globalClock = GetGlobalClock();
    handler->PSendSysMessage(
        "Global environment: time {}, speed x{:.2f}.",
        FormatClock(globalClock),
        globalClock.Speed);

    if (GlobalState.Weather)
    {
        handler->PSendSysMessage(
            "Global weather: {} ({:.2f}), abrupt={}.",
            GetWeatherName(GlobalState.Weather->State),
            GlobalState.Weather->Intensity,
            GlobalState.Weather->Abrupt ? "yes" : "no");
    }
    else
        handler->SendSysMessage("Global weather: natural.");

    if (GlobalState.Light)
    {
        handler->PSendSysMessage(
            "Global light: ID {}, transition {} ms.",
            GlobalState.Light->LightId,
            GlobalState.Light->FadeMilliseconds);
    }
    else
        handler->SendSysMessage("Global light: natural.");

    if (GlobalState.Music)
        handler->PSendSysMessage("Global music: ID {}.", *GlobalState.Music);
    else
        handler->SendSysMessage("Global music: natural.");

    if (!includeZone)
        return;

    auto const zone = ZoneStates.find(zoneId);
    handler->PSendSysMessage("Current zone: ID {}.", zoneId);
    if (zone == ZoneStates.end())
    {
        handler->SendSysMessage(
            "The zone inherits the complete global environment.");
        return;
    }

    if (zone->second.Clock)
    {
        handler->PSendSysMessage(
            "Zone time: {}, speed x{:.2f}.",
            FormatClock(*zone->second.Clock),
            zone->second.Clock->Speed);
    }
    else
        handler->SendSysMessage("Zone time: inherited from global.");

    if (zone->second.Weather)
    {
        handler->PSendSysMessage(
            "Zone weather: {} ({:.2f}), abrupt={}.",
            GetWeatherName(zone->second.Weather->State),
            zone->second.Weather->Intensity,
            zone->second.Weather->Abrupt ? "yes" : "no");
    }
    else
        handler->SendSysMessage("Zone weather: inherited from global.");

    if (zone->second.Light)
    {
        handler->PSendSysMessage(
            "Zone light: ID {}, transition {} ms.",
            zone->second.Light->LightId,
            zone->second.Light->FadeMilliseconds);
    }
    else
        handler->SendSysMessage("Zone light: inherited from global.");

    if (zone->second.Music)
        handler->PSendSysMessage(
            "Zone music: ID {}.", *zone->second.Music);
    else
        handler->SendSysMessage("Zone music: inherited from global.");
}

void PrintEnvironmentHelp(ChatHandler* handler)
{
    handler->SendSysMessage(".environment status");
    handler->SendSysMessage(
        ".environment time global|zone <HH:MM|dawn|noon|dusk|night|midnight>");
    handler->SendSysMessage(
        ".environment time resetglobal|resetzone");
    handler->SendSysMessage(
        ".environment speed global|zone <0.1-1440>");
    handler->SendSysMessage(
        ".environment speed resetglobal|resetzone");
    handler->SendSysMessage(
        ".environment weather global|zone <clear|fog|rain|snow|sandstorm|thunder|blackrain|blacksnow> [intensity 0-1] [abrupt 0/1]");
    handler->SendSysMessage(
        ".environment weather resetglobal|resetzone");
    handler->SendSysMessage(
        ".environment light global|zone <lightId> [transitionMs]");
    handler->SendSysMessage(
        ".environment light resetglobal|resetzone");
    handler->SendSysMessage(
        ".environment music global|zone <musicId>");
    handler->SendSysMessage(
        ".environment music resetglobal|resetzone");
    handler->SendSysMessage(
        ".environment reset global|zone");
    handler->SendSysMessage(".environment sync");
}

class FastDayNightWorldScript : public WorldScript
{
public:
    FastDayNightWorldScript() : WorldScript(
        "FastDayNightWorldScript",
        {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_UPDATE
        }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig();

        LOG_INFO(
            "module.fastdaynight",
            "FastEnvironment: configuration {}. Enabled={}, default speed=x{:.2f}, maximum=x{:.2f}, offset={}h.",
            reload ? "reloaded" : "loaded",
            Config.Enabled ? "yes" : "no",
            Config.DefaultSpeed,
            Config.MaximumSpeed,
            Config.OffsetHours);

        if (reload && RuntimeInitialized)
            RefreshAppliedEnvironment();
    }

    void OnStartup() override
    {
        LoadPersistentState();
        RefreshAppliedEnvironment();

        LOG_INFO(
            "module.fastdaynight",
            "FastEnvironment: persistent state loaded. Zone overrides={}, global clock={}, speed=x{:.2f}.",
            ZoneStates.size(),
            FormatClock(GetGlobalClock()),
            GetGlobalClock().Speed);
    }

    void OnUpdate(uint32 diff) override
    {
        if (!Config.Enabled)
            return;

        SyncTimer += diff;
        uint32 const interval =
            Config.SyncIntervalSeconds * IN_MILLISECONDS;
        if (SyncTimer < interval)
            return;

        SyncTimer %= interval;
        SyncAllPlayerTimes();
    }
};

class FastDayNightPlayerScript : public PlayerScript
{
public:
    FastDayNightPlayerScript() : PlayerScript(
        "FastDayNightPlayerScript",
        {
            PLAYERHOOK_ON_SEND_INITIAL_PACKETS_BEFORE_ADD_TO_MAP,
            PLAYERHOOK_ON_UPDATE_ZONE
        }) { }

    void OnPlayerSendInitialPacketsBeforeAddToMap(
        Player* player, WorldPacket& /*data*/) override
    {
        SendConfiguredTime(player);
    }

    void OnPlayerUpdateZone(
        Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        ApplyEnvironment(player);
    }
};

class FastDayNightCommandScript : public CommandScript
{
public:
    FastDayNightCommandScript() :
        CommandScript("FastDayNightCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable timeCommands =
        {
            { "global", HandleGlobalTime, EnvironmentCommandSecurity, Console::Yes },
            { "zone", HandleZoneTime, EnvironmentCommandSecurity, Console::No },
            { "resetglobal", HandleResetGlobalTime, EnvironmentCommandSecurity, Console::Yes },
            { "resetzone", HandleResetZoneTime, EnvironmentCommandSecurity, Console::No }
        };

        static ChatCommandTable speedCommands =
        {
            { "global", HandleGlobalSpeed, EnvironmentCommandSecurity, Console::Yes },
            { "zone", HandleZoneSpeed, EnvironmentCommandSecurity, Console::No },
            { "resetglobal", HandleResetGlobalSpeed, EnvironmentCommandSecurity, Console::Yes },
            { "resetzone", HandleResetZoneSpeed, EnvironmentCommandSecurity, Console::No }
        };

        static ChatCommandTable weatherCommands =
        {
            { "global", HandleGlobalWeather, EnvironmentCommandSecurity, Console::Yes },
            { "zone", HandleZoneWeather, EnvironmentCommandSecurity, Console::No },
            { "resetglobal", HandleResetGlobalWeather, EnvironmentCommandSecurity, Console::Yes },
            { "resetzone", HandleResetZoneWeather, EnvironmentCommandSecurity, Console::No }
        };

        static ChatCommandTable lightCommands =
        {
            { "global", HandleGlobalLight, EnvironmentCommandSecurity, Console::Yes },
            { "zone", HandleZoneLight, EnvironmentCommandSecurity, Console::No },
            { "resetglobal", HandleResetGlobalLight, EnvironmentCommandSecurity, Console::Yes },
            { "resetzone", HandleResetZoneLight, EnvironmentCommandSecurity, Console::No }
        };

        static ChatCommandTable musicCommands =
        {
            { "global", HandleGlobalMusic, EnvironmentCommandSecurity, Console::Yes },
            { "zone", HandleZoneMusic, EnvironmentCommandSecurity, Console::No },
            { "resetglobal", HandleResetGlobalMusic, EnvironmentCommandSecurity, Console::Yes },
            { "resetzone", HandleResetZoneMusic, EnvironmentCommandSecurity, Console::No }
        };

        static ChatCommandTable resetCommands =
        {
            { "global", HandleResetGlobalAll, EnvironmentCommandSecurity, Console::Yes },
            { "zone", HandleResetZoneAll, EnvironmentCommandSecurity, Console::No }
        };

        static ChatCommandTable environmentCommands =
        {
            { "help", HandleHelp, EnvironmentCommandSecurity, Console::Yes },
            { "status", HandleStatus, EnvironmentCommandSecurity, Console::Yes },
            { "time", timeCommands },
            { "speed", speedCommands },
            { "weather", weatherCommands },
            { "light", lightCommands },
            { "music", musicCommands },
            { "reset", resetCommands },
            { "sync", HandleSync, EnvironmentCommandSecurity, Console::Yes },
            { "", HandleHelp, EnvironmentCommandSecurity, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "environment", environmentCommands },
            { "env", environmentCommands }
        };
        return commandTable;
    }

private:
    static bool HandleHelp(ChatHandler* handler)
    {
        PrintEnvironmentHelp(handler);
        return true;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        PrintEnvironmentStatus(
            handler,
            player ? player->GetZoneId() : 0,
            player && player->IsInWorld());
        return true;
    }

    static bool HandleSync(ChatHandler* handler)
    {
        RefreshAppliedEnvironment();
        handler->SendSysMessage(
            "Time and environment synchronized for all online players.");
        return true;
    }

    static bool HandleResetGlobalAll(ChatHandler* handler)
    {
        GlobalState = {};
        GlobalState.Clock = MakeDefaultClock();
        SaveGlobalState();
        RefreshAppliedEnvironment();
        handler->SendSysMessage(
            "Global environment reset to configuration defaults and natural weather.");
        return true;
    }

    static bool HandleResetZoneAll(ChatHandler* handler)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        ZoneStates[zoneId] = {};
        SaveZoneState(zoneId);
        RefreshAppliedEnvironment();
        handler->PSendSysMessage(
            "Zone {} now inherits the complete global environment.",
            zoneId);
        return true;
    }

    static bool SetClockTime(
        ChatHandler* handler, ClockState& clock, std::string value)
    {
        uint8 hour = 0;
        uint8 minute = 0;
        if (!ParseClockTime(value, hour, minute))
        {
            handler->SendSysMessage(
                "Invalid time. Use HH:MM or dawn, noon, dusk, night, or midnight.");
            return false;
        }

        time_t visualTime = SetVisualClockTime(
            GetVisualTime(clock), hour, minute);
        ReanchorClock(clock, visualTime, clock.Speed);
        return true;
    }

    static bool HandleGlobalTime(
        ChatHandler* handler, std::string value)
    {
        ClockState clock = GetGlobalClock();
        if (!SetClockTime(handler, clock, std::move(value)))
            return false;

        GlobalState.Clock = clock;
        SaveGlobalState();
        SyncAllPlayerTimes();
        handler->PSendSysMessage(
            "Global time set to {}. The cycle continues at speed x{:.2f}.",
            FormatClock(clock), clock.Speed);
        return true;
    }

    static bool HandleZoneTime(
        ChatHandler* handler, std::string value)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        EnvironmentState& zone = GetZoneState(zoneId);
        ClockState clock = zone.Clock
            ? *zone.Clock
            : GetEffectiveClock(zoneId);
        if (!SetClockTime(handler, clock, std::move(value)))
            return false;

        zone.Clock = clock;
        SaveZoneState(zoneId);
        SendConfiguredTime(player);
        handler->PSendSysMessage(
            "Zone {} time set to {}. It continues at speed x{:.2f}.",
            zoneId, FormatClock(clock), clock.Speed);
        return true;
    }

    static bool HandleResetGlobalTime(ChatHandler* handler)
    {
        GlobalState.Clock = MakeDefaultClock();
        SaveGlobalState();
        SyncAllPlayerTimes();
        handler->PSendSysMessage(
            "Global clock reset to configuration: {}, speed x{:.2f}.",
            FormatClock(GetGlobalClock()), GetGlobalClock().Speed);
        return true;
    }

    static bool HandleResetZoneTime(ChatHandler* handler)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        GetZoneState(zoneId).Clock.reset();
        SaveZoneState(zoneId);
        SendConfiguredTime(player);
        handler->PSendSysMessage(
            "Zone {} now inherits global time.", zoneId);
        return true;
    }

    static bool SetSpeed(
        ChatHandler* handler, ClockState& clock, float speed)
    {
        if (!std::isfinite(speed) || speed < 0.1f ||
            speed > Config.MaximumSpeed)
        {
            handler->PSendSysMessage(
                "Invalid speed. Allowed range: 0.1 to {:.2f}.",
                Config.MaximumSpeed);
            return false;
        }

        ReanchorClock(clock, GetVisualTime(clock), speed);
        return true;
    }

    static bool HandleGlobalSpeed(
        ChatHandler* handler, float speed)
    {
        ClockState clock = GetGlobalClock();
        if (!SetSpeed(handler, clock, speed))
            return false;

        GlobalState.Clock = clock;
        SaveGlobalState();
        SyncAllPlayerTimes();
        handler->PSendSysMessage(
            "Global speed set to x{:.2f}; current visual time was preserved.",
            clock.Speed);
        return true;
    }

    static bool HandleZoneSpeed(
        ChatHandler* handler, float speed)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        EnvironmentState& zone = GetZoneState(zoneId);
        ClockState clock = zone.Clock
            ? *zone.Clock
            : GetEffectiveClock(zoneId);
        if (!SetSpeed(handler, clock, speed))
            return false;

        zone.Clock = clock;
        SaveZoneState(zoneId);
        SendConfiguredTime(player);
        handler->PSendSysMessage(
            "Zone {} speed set to x{:.2f}.",
            zoneId, clock.Speed);
        return true;
    }

    static bool HandleResetGlobalSpeed(ChatHandler* handler)
    {
        ClockState clock = GetGlobalClock();
        ReanchorClock(
            clock, GetVisualTime(clock), Config.DefaultSpeed);
        GlobalState.Clock = clock;
        SaveGlobalState();
        SyncAllPlayerTimes();
        handler->PSendSysMessage(
            "Global speed reset to x{:.2f}.",
            clock.Speed);
        return true;
    }

    static bool HandleResetZoneSpeed(ChatHandler* handler)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        EnvironmentState& zone = GetZoneState(zoneId);
        if (!zone.Clock)
        {
            handler->SendSysMessage(
                "The zone already inherits global speed.");
            return true;
        }

        ReanchorClock(
            *zone.Clock,
            GetVisualTime(*zone.Clock),
            GetGlobalClock().Speed);
        SaveZoneState(zoneId);
        SendConfiguredTime(player);
        handler->PSendSysMessage(
            "Zone {} speed matched to global x{:.2f}. "
            "Use '.environment time resetzone' to remove its independent clock.",
            zoneId, zone.Clock->Speed);
        return true;
    }

    static bool BuildWeather(
        ChatHandler* handler, std::string type,
        Optional<float> intensityArg, Optional<bool> abruptArg,
        WeatherOverride& weather)
    {
        float intensity = ClampIntensity(intensityArg.value_or(0.65f));
        std::optional<WeatherState> state =
            ParseWeatherState(std::move(type), intensity);
        if (!state)
        {
            handler->SendSysMessage(
                "Invalid weather. Use clear, fog, rain, snow, sandstorm, thunder, blackrain, or blacksnow.");
            return false;
        }

        if (*state == WEATHER_STATE_FINE)
            intensity = 0.0f;

        weather = { *state, intensity, abruptArg.value_or(false) };
        return true;
    }

    static bool HandleGlobalWeather(
        ChatHandler* handler, std::string type,
        Optional<float> intensity, Optional<bool> abrupt)
    {
        WeatherOverride weather;
        if (!BuildWeather(
                handler, std::move(type), intensity, abrupt, weather))
            return false;

        GlobalState.Weather = weather;
        SaveGlobalState();
        RefreshAppliedEnvironment();
        handler->PSendSysMessage(
            "Global weather: {} with intensity {:.2f}.",
            GetWeatherName(weather.State), weather.Intensity);
        return true;
    }

    static bool HandleZoneWeather(
        ChatHandler* handler, std::string type,
        Optional<float> intensity, Optional<bool> abrupt)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        WeatherOverride weather;
        if (!BuildWeather(
                handler, std::move(type), intensity, abrupt, weather))
            return false;

        GetZoneState(zoneId).Weather = weather;
        SaveZoneState(zoneId);
        ApplyPersistentEnvironment(player->FindMap(), zoneId);
        handler->PSendSysMessage(
            "Zone {} weather: {} with intensity {:.2f}.",
            zoneId, GetWeatherName(weather.State), weather.Intensity);
        return true;
    }

    static bool HandleResetGlobalWeather(ChatHandler* handler)
    {
        GlobalState.Weather.reset();
        SaveGlobalState();
        RefreshAppliedEnvironment();
        handler->SendSysMessage(
            "Global weather reset. Zones without overrides return to natural weather.");
        return true;
    }

    static bool HandleResetZoneWeather(ChatHandler* handler)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        GetZoneState(zoneId).Weather.reset();
        SaveZoneState(zoneId);
        RefreshAppliedEnvironment();
        handler->PSendSysMessage(
            "Zone {} now inherits global or natural weather.",
            zoneId);
        return true;
    }

    static bool HandleGlobalLight(
        ChatHandler* handler, Optional<uint32> lightId,
        Optional<uint32> fadeMilliseconds)
    {
        if (!lightId)
        {
            if (GlobalState.Light)
            {
                handler->PSendSysMessage(
                    "Global light: ID {}, transition {} ms.",
                    GlobalState.Light->LightId,
                    GlobalState.Light->FadeMilliseconds);
            }
            else
                handler->SendSysMessage("Global light: natural.");

            handler->SendSysMessage(
                "Usage: .environment light global <lightId> [transitionMs]");
            return true;
        }

        GlobalState.Light = LightOverride{
            *lightId,
            std::min<uint32>(
                fadeMilliseconds.value_or(
                    Config.DefaultLightFadeMilliseconds),
                60000)
        };
        SaveGlobalState();
        RefreshAppliedEnvironment();
        handler->PSendSysMessage(
            "Global light ID {} applied.", *lightId);
        return true;
    }

    static bool HandleZoneLight(
        ChatHandler* handler, Optional<uint32> lightId,
        Optional<uint32> fadeMilliseconds)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        if (!lightId)
        {
            auto const zone = ZoneStates.find(zoneId);
            if (zone != ZoneStates.end() && zone->second.Light)
            {
                handler->PSendSysMessage(
                    "Zone {} light: ID {}, transition {} ms.",
                    zoneId,
                    zone->second.Light->LightId,
                    zone->second.Light->FadeMilliseconds);
            }
            else
            {
                handler->PSendSysMessage(
                    "Zone {} light: inherited from global or natural.",
                    zoneId);
            }

            handler->SendSysMessage(
                "Usage: .environment light zone <lightId> [transitionMs]");
            return true;
        }

        GetZoneState(zoneId).Light = LightOverride{
            *lightId,
            std::min<uint32>(
                fadeMilliseconds.value_or(
                    Config.DefaultLightFadeMilliseconds),
                60000)
        };
        SaveZoneState(zoneId);
        ApplyPersistentEnvironment(player->FindMap(), zoneId);
        handler->PSendSysMessage(
            "Light ID {} applied to zone {}.", *lightId, zoneId);
        return true;
    }

    static bool HandleResetGlobalLight(ChatHandler* handler)
    {
        GlobalState.Light.reset();
        SaveGlobalState();
        RefreshAppliedEnvironment();
        handler->SendSysMessage(
            "Global lighting reset to natural behavior.");
        return true;
    }

    static bool HandleResetZoneLight(ChatHandler* handler)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        GetZoneState(zoneId).Light.reset();
        SaveZoneState(zoneId);
        RefreshAppliedEnvironment();
        handler->PSendSysMessage(
            "Zone {} now inherits global or natural lighting.",
            zoneId);
        return true;
    }

    static bool HandleGlobalMusic(
        ChatHandler* handler, uint32 musicId)
    {
        GlobalState.Music = musicId;
        SaveGlobalState();
        RefreshAppliedEnvironment();
        handler->PSendSysMessage(
            "Global music ID {} applied.", musicId);
        return true;
    }

    static bool HandleZoneMusic(
        ChatHandler* handler, uint32 musicId)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        GetZoneState(zoneId).Music = musicId;
        SaveZoneState(zoneId);
        ApplyPersistentEnvironment(player->FindMap(), zoneId);
        handler->PSendSysMessage(
            "Music ID {} applied to zone {}.", musicId, zoneId);
        return true;
    }

    static bool HandleResetGlobalMusic(ChatHandler* handler)
    {
        GlobalState.Music.reset();
        SaveGlobalState();
        RefreshAppliedEnvironment();
        handler->SendSysMessage(
            "Global music reset to natural behavior.");
        return true;
    }

    static bool HandleResetZoneMusic(ChatHandler* handler)
    {
        Player* player = nullptr;
        uint32 zoneId = 0;
        if (!RequirePlayerZone(handler, player, zoneId))
            return false;

        GetZoneState(zoneId).Music.reset();
        SaveZoneState(zoneId);
        RefreshAppliedEnvironment();
        handler->PSendSysMessage(
            "Zone {} now inherits global or natural music.",
            zoneId);
        return true;
    }
};
}

void Addmod_fast_day_nightScripts()
{
    new FastDayNightWorldScript();
    new FastDayNightPlayerScript();
    new FastDayNightCommandScript();
}
