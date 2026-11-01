/* bro-scenario-world.h: BroScenarioWorld, the world a scenario starts in (decision 235), on the test root: {root} is
 * TEST_ROOT and paths use '/' everywhere. The configuration (given.config, else config3-scenario-base.json, then
 * configPatch, then profile over the default profile, {root} filled in) is written to the data folder before the engine
 * starts; each library gets its folder and marker; given.files are written; given.disc is what the ISO (and a drive given
 * no disc) holds, and BRO_SCENARIO_ISO exists as a small file. The drive world (decision 237): each fake drive's device,
 * identification, MakeMKV index, tray and disc (BroScenarioDrive), which the drive scan's DRV lines report (no disc while
 * the tray is open); the engine's eject opens the tray, leaving the disc in it (the OS reports TrayOpened), and its close
 * shuts it unless the drive has no tray motor. The tools are the scripts (given.tools; makemkvcon's default is
 * tools/makemkvcon-saves.yaml). Thread-safe where the tools and the fake drives call it. Test code, not shipped. */
#pragma once

#include "bro-listing.h"
#include "bro-test-composition-root.h"

G_BEGIN_DECLS

#define BRO_SCENARIO_ISO TEST_ROOT "/test.iso"

/* A fake drive: its scenario name, device, identification, MakeMKV index, tray and disc. */
typedef struct {
  char *name;
  char *device;
  char *identification;
  int index;
  gboolean tray_open;
  gboolean tray_motor;
  BroJsonValue *disc; /* nullable */
} BroScenarioDrive;

typedef struct {
  GMutex lock;
  BroJsonValue *given;
  GPtrArray *drives;    /* BroScenarioDrive * */
  GHashTable *scripts;  /* tool → BroJsonValue */
  GHashTable *markers;  /* library id → its marker's id */
  BroJsonValue *disc;   /* nullable */
  TestCompositionRoot *root; /* borrowed, once prepared */
} BroScenarioWorld;

/* "<expected> / <found>" (free with g_free): how every part of the runner says what didn't hold. */
char *bro_scenario_failure (const char *expected, const char *found);

BroScenarioWorld *bro_scenario_world_new (BroJsonValue *scenario);
void bro_scenario_world_free (BroScenarioWorld *world);

/* Whether the tool has a script (the locator finds exactly those). */
gboolean bro_scenario_world_installed (BroScenarioWorld *world, const char *tool);
/* The tool's script from now on (NULL or null: not installed). */
void bro_scenario_world_set_script (BroScenarioWorld *world, const char *tool, BroJsonValue *script);
/* The tool's script (a reference; NULL: none). */
BroJsonValue *bro_scenario_world_script (BroScenarioWorld *world, const char *tool);
/* What the ISO (and a folder or file source) holds (a reference; NULL: none); set replaces it. */
BroJsonValue *bro_scenario_world_disc (BroScenarioWorld *world);
void bro_scenario_world_set_disc (BroScenarioWorld *world, BroJsonValue *disc);
/* The drive of that scenario name (borrowed; NULL with *@failure set when there is none). */
BroScenarioDrive *bro_scenario_world_drive_named (BroScenarioWorld *world, const char *name, char **failure);

/* Writes the configuration, libraries, files, units and the ISO; adds the drives and hooks the tray calls. FALSE with
 * *@failure set when a given unit can't be made. */
gboolean bro_scenario_world_prepare (BroScenarioWorld *world, TestCompositionRoot *root, char **failure);
/* The configuration document the engine starts with. */
BroJsonValue *bro_scenario_world_configuration (BroScenarioWorld *world);

/* RFC 7386. */
BroJsonValue *bro_scenario_merge_patch (BroJsonValue *target, BroJsonValue *patch);
/* {root} in every string. */
BroJsonValue *bro_scenario_fill_root (BroJsonValue *v);
/* A file's bytes from a scenario: text, or {bytes, seed, descriptor} (that many bytes of a seeded pattern, a volume
 * descriptor at 32769). */
GBytes *bro_scenario_file_bytes (const BroJsonValue *content);

/* A disc goes into the drive (its tray closes) and the OS reports it; remove takes it out; open_tray opens the tray.
 * FALSE with *@failure set for a drive the scenario doesn't have. */
gboolean bro_scenario_world_insert (BroScenarioWorld *world, const char *name, BroJsonValue *disc, char **failure);
gboolean bro_scenario_world_remove (BroScenarioWorld *world, const char *name, char **failure);
gboolean bro_scenario_world_open_tray (BroScenarioWorld *world, const char *name, char **failure);

/* The DRV lines of a drive scan (a strv): index, state (2 a disc, 1 an open tray, 0 empty), the disc's flags (DVD 1,
 * HD DVD 2, Blu-ray 4 + AACS 8), identification, label and device. */
GStrv bro_scenario_world_drive_scan (BroScenarioWorld *world);
/* The disc a makemkvcon source names: disc:N or dev:<device> a drive's, anything else the ISO's (a reference). */
BroJsonValue *bro_scenario_world_disc_of (BroScenarioWorld *world, const char *source);
/* The device a makemkvcon source names ("" for an image). */
char *bro_scenario_world_device_of (BroScenarioWorld *world, const char *source);

/* A disc's listing (README "A generated listing", or its listing fixture), without the titles shorter than @min_length
 * seconds (< 0: none left out), which renumbers the rest. */
char *bro_scenario_listing (const BroJsonValue *disc, int min_length);
/* The listing as the engine reads it (bro_robot_parse_line, BroListingBuilder). */
BroListing *bro_scenario_parsed_listing (const BroJsonValue *disc);
/* h:mm:ss as seconds. */
int bro_scenario_seconds (const char *duration);

G_END_DECLS
