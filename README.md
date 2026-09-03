# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore Module: mod-fast-day-night

[![AzerothCore Module](https://img.shields.io/badge/AzerothCore-Module-red?style=flat-square&logo=github)](https://github.com/azerothcore/azerothcore-wotlk)
[![C++20](https://img.shields.io/badge/Language-C++20-00599C?style=flat-square&logo=c%2B%2B)](https://isocpp.org/)
[![Branch 3.3.5a](https://img.shields.io/badge/Branch-3.3.5a-orange?style=flat-square)](https://github.com/azerothcore/azerothcore-wotlk)
[![License MIT](https://img.shields.io/badge/License-MIT-blue?style=flat-square)](https://opensource.org/licenses/MIT)

An advanced accelerated visual day/night cycle, dynamic weather, custom lighting, and ambient environment controller module for **AzerothCore (WotLK 3.3.5a)**.

### 💡 Why this module?
In default World of Warcraft, the real-time 24-hour cycle means players who log in after work or school in the evening are permanently stuck playing in the dark. 

**`mod-fast-day-night`** solves this by accelerating the visual day/night cycle (by default, 4 hours of daylight and 4 hours of night) and giving administrators full in-game command control over the time, weather, lighting, and music without desyncing server cooldowns, auctions, or event timers.

## 📊 Feature Comparison

| Feature | Standard Core Time | mod-fast-day-night |
| :--- | :---: | :---: |
| **Day/Night Cycle Speed** | ❌ Fixed 24-hour cycle (often always night for evening players) | ✅ **Configurable acceleration (default 3x: 4h day / 4h night)** |
| **Server Timers & Cooldowns** | ⚠️ Altering system clock breaks timers & auctions | ✅ **100% Safe: Only alters client visual clock packet (`SMSG_LOGIN_SETTIMESPEED`)** |
| **Live In-Game Time Shifting** | ❌ Requires server restart or OS clock changes | ✅ **Instant GM commands (`.env time global 06:00` or `dawn`)** |
| **Per-Zone Weather Overrides** | ❌ Global or fixed DBC scripts only | ✅ **Dynamic intensity & gradual fade per zone or globally** |
| **Custom Lighting & Skybox** | ❌ Requires client MPQ patch | ✅ **Server-side Light ID overrides with smooth transitions** |
| **Persistence Across Restarts** | ❌ Lost on server reboot | ✅ **Automatic database storage in `fast_environment_state`** |

## ⚙️ Technical Architecture

### 1. Visual Clock Acceleration Without Gameplay Interference
Traditional core modifications that attempt to alter game time inadvertently break creature respawns, auction house expiration, calendar events, and instance lockout resets.

`mod-fast-day-night` decouples the **visual client clock** from the **server game loop**:
- Sends accelerated game speed packets (`SMSG_LOGIN_SETTIMESPEED`) calculated via `ClockState` anchor offsets.
- Periodic background resync (`FastDayNight.SyncIntervalSeconds`) prevents client-side floating-point drift without stutter.
- All gameplay systems (spells, cooldowns, auctions, movement, databases) remain locked to genuine epoch time (`GameTime::GetGameTime()`).

### 2. Hierarchical Scope Engine (Global vs Zone)
Environment state uses a layered fallback hierarchy:
- **Zone Overrides:** GMs can apply specific visual time, weather, lighting, or music to an individual zone (e.g. perpetual thunderstorm over Elwynn Forest).
- **Global Settings:** When a zone override is reset, it seamlessly reverts to inheriting the global accelerated environment.

### 3. State Persistence
All runtime modifications made via GM commands are asynchronously persisted into the `fast_environment_state` database table, ensuring custom weather, time anchors, and lighting survive scheduled server restarts.

## 💬 In-Game Commands (`.environment` / `.env`)

> **Required Permission:** `SEC_ADMINISTRATOR` (Security Level 3) or Server Console.

| Command | Arguments | Description |
| :--- | :--- | :--- |
| `.env status` | - | Displays active global and zone environment states. |
| `.env time global <time>` | `06:00`, `dawn`, `noon`, `dusk`, `night` | Sets global visual clock and re-anchors cycle. |
| `.env time zone <time>` | `06:00`, `dawn`, `noon`, `dusk`, `night` | Sets visual clock override for GM's current zone. |
| `.env speed global <mult>` | `1` to `288` (e.g. `3` = 8h day) | Adjusts global day/night cycle speed multiplier. |
| `.env speed zone <mult>` | `1` to `288` | Adjusts speed multiplier for GM's current zone. |
| `.env weather global <type> <intensity> [abrupt]` | `rain 0.65 0`, `fog 0.5 0`, `snow 0.9 1` | Sets global weather with intensity (0.0-1.0) and fade mode. |
| `.env weather zone <type> <intensity> [abrupt]` | `thunder`, `sandstorm`, `blackrain` | Sets weather override for GM's current zone. |
| `.env light global <lightId> [fadeMs]` | Light ID (e.g. `840`), transition ms | Applies global skybox/lighting with smooth fade. |
| `.env light zone <lightId> [fadeMs]` | Light ID, transition ms | Applies lighting override for GM's current zone. |
| `.env music zone <musicId>` | Music SoundEntry ID | Overrides background music in current zone. |
| `.env reset zone` | - | Clears all zone overrides and restores global settings. |
| `.env reset global` | - | Clears all global overrides and restores natural behavior. |

## 📋 Configuration Reference (`FastDayNight.conf`)

```ini
[worldserver]

# Enable accelerated visual day/night cycle
FastDayNight.Enable = 1

# Visual time multiplier

# 1 = 24h real day, 2 = 12h day, 3 = 8h day (4h daylight / 4h night), 24 = 1h day
FastDayNight.Speed = 3

# Maximum multiplier accepted by in-game GM commands
FastDayNight.MaximumSpeed = 288

# Hour offset applied after acceleration (-23 to 23)
FastDayNight.OffsetHours = 0

# Sync interval to correct client visual clock drift (seconds)
FastDayNight.SyncIntervalSeconds = 300

# Default fade duration for light changes (milliseconds)
FastDayNight.DefaultLightFadeMilliseconds = 3000
```

## 🛠️ Installation

1. Place the module in your `modules/` directory:
   ```bash
   cd azerothcore-wotlk/modules
   git clone https://github.com/AlsoNotMehh/mod-fast-day-night.git
   ```

2. Import the database schema into your `acore_world` database:
   ```bash
   # AzerothCore will automatically import files from data/sql/db-world/ on startup
   ```

3. Re-run CMake and compile your server:
   ```bash
   cmake -B build
   cmake --build build --config Release
   ```

4. Copy `conf/FastDayNight.conf.dist` to your `worldserver` configs directory as `FastDayNight.conf`.

## ⭐ Show your support

If you find this module helpful for your server, please consider giving it a star on GitHub! It helps more developers in the AzerothCore community discover the project.

## 🤝 Credits

- **Author:** [AlsoNotMehh](https://github.com/AlsoNotMehh) ([Discord](https://discord.com/users/1063304041419001966) / [Email](mailto:itsbrayanrodriguez@gmail.com))
- **Framework:** [AzerothCore](https://www.azerothcore.org)

## 📜 License

This project is licensed under the [MIT License](LICENSE).
