CREATE DATABASE IF NOT EXISTS `Custom`
    DEFAULT CHARACTER SET utf8mb4
    COLLATE utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `Custom`.`fast_environment_state` (
    `scope_type` TINYINT UNSIGNED NOT NULL,
    `scope_id` INT UNSIGNED NOT NULL,
    `clock_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `visual_anchor` BIGINT NOT NULL DEFAULT 0,
    `real_anchor` BIGINT NOT NULL DEFAULT 0,
    `speed` DOUBLE NOT NULL DEFAULT 1,
    `weather_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `weather_state` INT UNSIGNED NOT NULL DEFAULT 0,
    `weather_intensity` FLOAT NOT NULL DEFAULT 0,
    `weather_abrupt` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `light_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `light_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `light_fade_ms` INT UNSIGNED NOT NULL DEFAULT 3000,
    `music_enabled` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `music_id` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`scope_type`, `scope_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
