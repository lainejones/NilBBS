/*
 * BBSConfig - the NilBBS configuration program (GadTools, Workbench).
 *
 * A section list on the left, an editor panel on the right:
 *   System, New users & time, Security, IP rules, Doors, Message areas,
 *   File areas, FidoNet, Menus, Users.
 * Config files are edited in place (comments kept, see ini.c).  Save writes
 * every changed file and asks a running NilBBS to reload its IP filter;
 * other settings apply to the next caller (port/nodes: restart NilBBS).
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <graphics/gfxmacros.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include <proto/asl.h>
#include <libraries/asl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "bbs.h"
#include "ini.h"
#include "lang.h"
#include "doorcheck.h"

static const char __attribute__((used)) verstag[] = "$VER: BBSConfig " BBS_VERSION " (" BBS_VERDATE ")";

struct IntuitionBase *IntuitionBase;
struct Library *GadToolsBase;
struct Library *AslBase;                /* optional: the menu-screen picker */
struct GfxBase *GfxBase;

/* ---- field descriptions ---------------------------------------------------- */

#define F_STR  0
#define F_INT  1
#define F_BOOL 2
#define F_CYC  3

#define W_HALF  0
#define W_FULL  1
#define W_THIRD 2

struct Field {
    const char *label;
    const char *key;
    UBYTE kind, width;
    const char *def;
    const char **cyc;          /* F_CYC: the values (also the labels) */
    WORD  maxc;
};

static const char *cyc_door_type[] = { "cli", "cnetrexx", "aim", "cnetc", "xim", "rlogin", "telnet", "tcp", NULL };
static const char *cyc_dropfile[]  = { "door.sys", "dorinfo1.def", "door32.sys", "all", "none", NULL };
static const char *cyc_charset[]   = { "cp437", "amiga", "latin1", NULL };
static const char *cyc_cnetver[]   = { "4", "3", NULL };
static const char *cyc_areatype[]  = { "local", "echo", "email", NULL };
/* the board's language: the .lng files in BBS:Text/Language (filled by lang_cycle_fill) */
#define MAX_LANGS 24
static char lang_nm[MAX_LANGS + 1][LANG_NAMELEN];
static const char *cyc_lang[MAX_LANGS + 2] = { LANG_ENGLISH, NULL };

static struct Field f_system[] = {
    { "BBS name",       "bbs_name",          F_STR,  W_FULL, "NilBBS", NULL, 40 },
    { "Sysop name",     "sysop_name",        F_STR,  W_HALF, "Sysop", NULL, 31 },
    { "Start menu",     "start_menu",        F_STR,  W_HALF, "main", NULL, 31 },
    { "Language",       "language",          F_CYC,  W_HALF, LANG_ENGLISH, cyc_lang, 0 },
    { "Telnet port",    "port",              F_INT,  W_HALF, "23", NULL, 5 },
    { "Nodes",          "nodes",             F_INT,  W_HALF, "4", NULL, 2 },
    { "Idle minutes",   "idle_minutes",      F_INT,  W_HALF, "10", NULL, 4 },
    { "Node program",   "node_command",      F_STR,  W_HALF, "BBS:BBSNode", NULL, 100 },
    { "Last callers",   "logon_lastcallers", F_BOOL, W_HALF, "yes", NULL, 0 },
    { "One-liners",     "logon_oneliners",   F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Hide sysop",     "hide_sysop_calls",  F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Hide from lvl",  "hide_calls_level",  F_INT,  W_HALF, "255", NULL, 3 },
    { "1-liner cap",  "oneliners_keep",    F_INT,  W_HALF, "50", NULL, 5 },
    { NULL }
};
static struct Field f_newusers[] = {
    { "Allow new",      "newuser_allowed",   F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Ask e-mail",     "ask_email",         F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Signup pass",    "newuser_password",  F_STR,  W_HALF, "", NULL, 30 },
    { "Max tries",      "max_login_tries",   F_INT,  W_HALF, "3", NULL, 2 },
    { "New level",      "newuser_level",     F_INT,  W_HALF, "10", NULL, 3 },
    { "Validated lvl",  "validated_level",   F_INT,  W_HALF, "0", NULL, 3 },
    { "Sysop level",    "sysop_level",       F_INT,  W_HALF, "255", NULL, 3 },
    { "Min by level",   "level_minutes",     F_STR,  W_FULL, "0:120,255:0", NULL, 120 },
    { NULL }
};
static struct Field f_security[] = {
    { "Flood conns",    "flood_connects",    F_INT,  W_HALF, "5", NULL, 4 },
    { "...in seconds",  "flood_seconds",     F_INT,  W_HALF, "30", NULL, 5 },
    { "Flood ban min",  "flood_ban_minutes", F_INT,  W_HALF, "60", NULL, 6 },
    { "Bad logins",     "failed_logins",     F_INT,  W_HALF, "5", NULL, 3 },
    { "Login ban min",  "failed_ban_minutes",F_INT,  W_HALF, "30", NULL, 6 },
    { "Trap handles",   "trap_names",        F_STR,  W_FULL, "admin root administrator", NULL, 150 },
    { "Deny message",   "deny_message",      F_STR,  W_FULL, "\\r\\nAccess denied.\\r\\n", NULL, 200 },
    { "Ban message",    "ban_message",       F_STR,  W_FULL, "\\r\\nYour address is banned from this system.\\r\\n", NULL, 200 },
    { "Busy message",   "busy_message",      F_STR,  W_FULL, "\\r\\nAll nodes are busy - please call back later.\\r\\n", NULL, 200 },
    { NULL }
};
/* Doors: three pages (Show: Door / Options / Hang-up), the Options page per door type -
 * only what that type uses (door.c door_get + where each option is read) */
static struct Field f_doors[] = {           /* Door: every type */
    { "Tag",            NULL,                F_STR,  W_HALF, "", NULL, 24 },
    { "Level",          "level",             F_INT,  W_HALF, "10", NULL, 3 },
    { "Name",           "name",              F_STR,  W_FULL, "", NULL, 47 },
    { "Type",           "type",              F_CYC,  W_HALF, "cli", cyc_door_type, 0 },
    { "Access ACS",     "acs",               F_STR,  W_HALF, "", NULL, 63 },
    { "Command",        "command",           F_STR,  W_FULL, "", NULL, 200 },
    { "Directory",      "dir",               F_STR,  W_FULL, "", NULL, 200 },
    { "Assign",         "assign",            F_STR,  W_FULL, "", NULL, 100 },
    { "Single",         "single",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Maint cmd",      "maint",             F_STR,  W_FULL, "", NULL, 200 },
    { NULL }
};
#define DOOR_TYPE_FIELD 3                   /* f_doors[3] is Type */
static struct Field f_dopt_cli[] = {        /* cli */
    { "Drop file",      "dropfile",          F_CYC,  W_HALF, "door.sys", cyc_dropfile, 0 },
    { "Charset",        "charset",           F_CYC,  W_HALF, "cp437", cyc_charset, 0 },
    { "Stack",          "stack",             F_INT,  W_HALF, "16384", NULL, 7 },
    { "Stale lock",     "stale_lock",        F_STR,  W_HALF, "", NULL, 63 },
    { "Amiga CSI",      "amigacsi",          F_BOOL, W_THIRD, "yes", NULL, 0 },
    { "Arrows 9B",      "arrows8",           F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Raw out",        "rawout",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { NULL }
};
static struct Field f_dopt_rexx[] = {       /* cnetrexx, aim */
    { "Charset",        "charset",           F_CYC,  W_HALF, "cp437", cyc_charset, 0 },
    { "Panic grace",    "panic_grace",       F_INT,  W_HALF, "10", NULL, 4 },
    { "Drop file",      "dropfile",          F_CYC,  W_HALF, "door.sys", cyc_dropfile, 0 },
    { "Stale lock",     "stale_lock",        F_STR,  W_HALF, "", NULL, 63 },
    { "Old MCI \\c1",   "old_mci",           F_BOOL, W_THIRD, "no", NULL, 0 },
    { "TX newline",     "transmit_newline",  F_BOOL, W_THIRD, "yes", NULL, 0 },
    { "Q quits",        "qquit",             F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Amiga CSI",      "amigacsi",          F_BOOL, W_THIRD, "yes", NULL, 0 },
    { "Arrows 9B",      "arrows8",           F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Raw out",        "rawout",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { NULL }
};
static struct Field f_dopt_cnetc[] = {      /* cnetc */
    { "Charset",        "charset",           F_CYC,  W_HALF, "cp437", cyc_charset, 0 },
    { "CNet ver",       "cnet_version",      F_CYC,  W_HALF, "4", cyc_cnetver, 0 },
    { "Stack",          "stack",             F_INT,  W_HALF, "16384", NULL, 7 },
    { "Panic grace",    "panic_grace",       F_INT,  W_HALF, "10", NULL, 4 },
    { "Drop file",      "dropfile",          F_CYC,  W_HALF, "door.sys", cyc_dropfile, 0 },
    { "Stale lock",     "stale_lock",        F_STR,  W_HALF, "", NULL, 63 },
    { NULL }
};
static struct Field f_dopt_xim[] = {        /* xim */
    { "Charset",        "charset",           F_CYC,  W_HALF, "amiga", cyc_charset, 0 },
    { "Stack",          "stack",             F_INT,  W_HALF, "16384", NULL, 7 },
    { "Panic grace",    "panic_grace",       F_INT,  W_HALF, "10", NULL, 4 },
    { "Drop file",      "dropfile",          F_CYC,  W_HALF, "door.sys", cyc_dropfile, 0 },
    { "Stale lock",     "stale_lock",        F_STR,  W_HALF, "", NULL, 63 },
    { "Debug log",      "debug",             F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Amiga CSI",      "amigacsi",          F_BOOL, W_THIRD, "yes", NULL, 0 },
    { "Arrows 9B",      "arrows8",           F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Raw out",        "rawout",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { NULL }
};
static struct Field f_dopt_rlogin[] = {     /* rlogin */
    { "Host",           "host",              F_STR,  W_HALF, "", NULL, 79 },
    { "Port",           "port",              F_INT,  W_HALF, "513", NULL, 5 },
    { "Rlogin user",    "rlogin_user",       F_STR,  W_HALF, "%h", NULL, 60 },
    { "Rlogin term",    "rlogin_term",       F_STR,  W_HALF, "ansi-bbs/115200", NULL, 60 },
    { "Charset",        "charset",           F_CYC,  W_HALF, "cp437", cyc_charset, 0 },
    { "Raw out",        "rawout",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { NULL }
};
static struct Field f_dopt_net[] = {        /* telnet, tcp */
    { "Host",           "host",              F_STR,  W_HALF, "", NULL, 79 },
    { "Port",           "port",              F_INT,  W_HALF, "23", NULL, 5 },
    { "Charset",        "charset",           F_CYC,  W_HALF, "cp437", cyc_charset, 0 },
    { "Raw out",        "rawout",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { NULL }
};
static const char *cyc_hupread[] = { "eof", "error", NULL };
static struct Field f_dhup[] = {            /* cli, xim: a caller who drops */
    { "After drop",     "hangup_read",       F_CYC,  W_HALF, "eof", cyc_hupread, 0 },
    { "Keys",           "hangup_keys",       F_STR,  W_HALF, "", NULL, 150 },
    { "Rounds",         "hangup_rounds",     F_INT,  W_HALF, "30", NULL, 4 },
    { "Grace secs",     "hangup_grace",      F_INT,  W_HALF, "90", NULL, 4 },
    { "Rule 1",         "hangup_1",          F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 2",         "hangup_2",          F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 3",         "hangup_3",          F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 4",         "hangup_4",          F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 5",         "hangup_5",          F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 6",         "hangup_6",          F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 7",         "hangup_7",          F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 8",         "hangup_8",          F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 9",         "hangup_9",          F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 10",        "hangup_10",         F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 11",        "hangup_11",         F_STR,  W_HALF, "", NULL, 118 },
    { "Rule 12",        "hangup_12",         F_STR,  W_HALF, "", NULL, 118 },
    { NULL }
};
static struct Field f_msgareas[] = {
    { "Tag",            NULL,                F_STR,  W_HALF, "", NULL, 24 },
    { "Type",           "type",              F_CYC,  W_HALF, "local", cyc_areatype, 0 },
    { "Name",           "name",              F_STR,  W_FULL, "", NULL, 47 },
    { "Read level",     "read",              F_INT,  W_HALF, "0", NULL, 3 },
    { "Write level",    "write",             F_INT,  W_HALF, "10", NULL, 3 },
    { "Echo tag",       "echotag",           F_STR,  W_HALF, "", NULL, 31 },
    { "Conference",     "conf",              F_STR,  W_HALF, "", NULL, 47 },
    { "Read ACS",       "acs",               F_STR,  W_HALF, "", NULL, 63 },
    { "Post ACS",       "post_acs",          F_STR,  W_HALF, "", NULL, 63 },
    { "Sub-op ACS",     "subop",             F_STR,  W_HALF, "", NULL, 63 },
    { "Hold below lvl", "hold_below",        F_INT,  W_HALF, "0", NULL, 3 },
    { "Hold first",     "hold_first",        F_INT,  W_HALF, "0", NULL, 3 },
    { "Purge days",     "purge_days",        F_INT,  W_HALF, "0", NULL, 5 },
    { "Max messages",   "max_msgs",          F_INT,  W_HALF, "0", NULL, 6 },
    { NULL }
};
static struct Field f_fileareas[] = {
    { "Tag",            NULL,                F_STR,  W_HALF, "", NULL, 24 },
    { "Name",           "name",              F_STR,  W_HALF, "", NULL, 47 },
    { "Path",           "path",              F_STR,  W_FULL, "BBS:Files/Uploads", NULL, 200 },
    { "Parent (tag)",   "parent",            F_STR,  W_HALF, "", NULL, 24 },
    { "Download lvl",   "download",          F_INT,  W_HALF, "10", NULL, 3 },
    { "Upload lvl",     "upload",            F_INT,  W_HALF, "10", NULL, 3 },
    { "Conference",     "conf",              F_STR,  W_HALF, "", NULL, 47 },
    { "Free area","free",              F_BOOL, W_HALF, "no", NULL, 0 },
    { "CD/DVD (read-only)", "readonly",  F_BOOL, W_HALF, "no", NULL, 0 },
    { "CD-ROM browser",     "cdrom",     F_BOOL, W_HALF, "no", NULL, 0 },
    { "Down ACS",   "acs",               F_STR,  W_HALF, "", NULL, 63 },
    { "Upload ACS",     "upload_acs",        F_STR,  W_HALF, "", NULL, 63 },
    { "Sub-op ACS",     "subop",             F_STR,  W_HALF, "", NULL, 63 },
    { NULL }
};
/* CD/DVD drives page: the file areas that are discs (cdrom = yes or readonly = yes) */
static struct Field f_cdareas[] = {
    { "Tag",            NULL,                F_STR,  W_HALF, "", NULL, 24 },
    { "Download lvl",   "download",          F_INT,  W_HALF, "10", NULL, 3 },
    { "Name",           "name",              F_STR,  W_FULL, "", NULL, 47 },
    { "Drive/path",     "path",              F_STR,  W_HALF, "CD0:", NULL, 200 },
    { "Browse the whole disc", "cdrom",      F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Conference",     "conf",              F_STR,  W_HALF, "", NULL, 47 },
    { "Parent (tag)",   "parent",            F_STR,  W_HALF, "", NULL, 24 },
    { "Down ACS",       "acs",               F_STR,  W_HALF, "", NULL, 63 },
    { NULL }
};
static struct Field f_confs[] = {
    { "Tag",            NULL,                F_STR,  W_HALF, "", NULL, 24 },
    { "Join ACS",   "acs",               F_STR,  W_HALF, "", NULL, 63 },
    { "Name",           "name",              F_STR,  W_FULL, "", NULL, 47 },
    { NULL }
};
static struct Field f_bulletins[] = {
    { "Tag",            NULL,                F_STR,  W_HALF, "", NULL, 24 },
    { "Read ACS",   "acs",               F_STR,  W_HALF, "", NULL, 63 },
    { "Title",          "name",              F_STR,  W_FULL, "", NULL, 47 },
    { "Screen file",    "file",              F_STR,  W_FULL, "Bulletins/", NULL, 63 },
    { NULL }
};
static struct Field f_events[] = {
    { "Tag",            NULL,                F_STR,  W_HALF, "", NULL, 24 },
    { "Days",           "days",              F_STR,  W_HALF, "daily", NULL, 40 },
    { "Time HH:MM",     "time",              F_STR,  W_HALF, "", NULL, 5 },
    { "Every (min)", "every",             F_INT,  W_HALF, "0", NULL, 4 },
    { "Command",        "command",           F_STR,  W_FULL, "", NULL, 200 },
    { "Exclusive",      "exclusive",         F_BOOL, W_HALF, "no", NULL, 0 },
    { "Warn (min)",     "warn",              F_INT,  W_HALF, "5", NULL, 3 },
    { NULL }
};
static struct Field f_maint[] = {
    { "Msg max days",   "maint_msg_days",    F_INT,  W_HALF, "0", NULL, 5 },
    { "Msgs per area",  "maint_msg_max",     F_INT,  W_HALF, "0", NULL, 6 },
    { "Read mail days", "maint_mail_days",   F_INT,  W_HALF, "180", NULL, 5 },
    { "Trash days",     "maint_trash_days",  F_INT,  W_HALF, "14", NULL, 4 },
    { "Retire after",   "maint_user_days",   F_INT,  W_HALF, "0", NULL, 5 },
    { "Keep level",     "maint_user_keep_level", F_INT, W_HALF, "100", NULL, 3 },
    { "Log size KB",    "maint_log_kb",      F_INT,  W_HALF, "256", NULL, 6 },
    { "Mail report",    "maint_report",      F_BOOL, W_HALF, "yes", NULL, 0 },
    { NULL }
};
static struct Field f_community[] = {
    { "Logon bulls","logon_bulletins",   F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Logon votes",   "logon_voting",      F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Vote add ACS", "vote_add_acs",      F_STR,  W_HALF, "S", NULL, 63 },
    { "Finger port",    "finger_port",       F_INT,  W_HALF, "0", NULL, 5 },
    { "Finger users",   "finger_users",      F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Real names",     "finger_realname",   F_BOOL, W_HALF, "no", NULL, 0 },
    { "QWK BBS id",     "qwk_bbsid",         F_STR,  W_HALF, "", NULL, 8 },
    { "QWK max",   "qwk_max",           F_INT,  W_HALF, "500", NULL, 5 },
    { "QWK pack",       "qwk_pack",          F_STR,  W_FULL, "LhA >NIL: -q -m a \"%a\" #?", NULL, 200 },
    { "Unpack LHA",     "qwk_unpack_lha",    F_STR,  W_FULL, "LhA >NIL: -q -m x \"%a\"", NULL, 200 },
    { "Unpack ZIP",     "qwk_unpack_zip",    F_STR,  W_FULL, "UnZip >NIL: -o -q \"%a\"", NULL, 200 },
    { NULL }
};
static struct Field f_files[] = {
    { "Offer DIZ",      "diz_offer_upload",  F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Strip ads",      "strip_ads",         F_BOOL, W_HALF, "yes", NULL, 0 },
    { "Strip LHA ad",   "strip_lha",         F_STR,  W_FULL, "LhA >NIL: -q d \"%a\" \"%f\"", NULL, 200 },
    { "Read LHA DIZ",   "diz_lha",           F_STR,  W_FULL, "LhA >NIL: -q -m e \"%a\" FILE_ID.DIZ \"%d/\"", NULL, 200 },
    { "Read ZIP DIZ",   "diz_zip",           F_STR,  W_FULL, "UnZip >NIL: -o -j -C \"%a\" FILE_ID.DIZ -d \"%d\"", NULL, 200 },
    { "Add LHA DIZ",    "diz_add_lha",       F_STR,  W_FULL, "LhA >NIL: -q -m a \"%a\" FILE_ID.DIZ", NULL, 200 },
    { "Add ZIP DIZ",    "diz_add_zip",       F_STR,  W_FULL, "Zip >NIL: -q -j \"%a\" FILE_ID.DIZ", NULL, 200 },
    { "Del LHA DIZ",    "diz_del_lha",       F_STR,  W_FULL, "LhA >NIL: -q -m d \"%a\" FILE_ID.DIZ", NULL, 200 },
    { "Del ZIP DIZ",    "diz_del_zip",       F_STR,  W_FULL, "Zip >NIL: -q -d \"%a\" FILE_ID.DIZ", NULL, 200 },
    { "Ratio N:1",   "ratio",             F_INT,  W_HALF, "0", NULL, 4 },
    { "Free KB",        "ratio_free_kb",     F_INT,  W_HALF, "1024", NULL, 7 },
    { "Ratio exempt",   "ratio_exempt",      F_STR,  W_HALF, "L100", NULL, 63 },
    { NULL }
};
static struct Field f_fido[] = {
    { "Address",        "address",           F_STR,  W_HALF, "", NULL, 23 },
    { "Uplink",         "uplink",            F_STR,  W_HALF, "", NULL, 23 },
    { "Password",       "password",          F_STR,  W_HALF, "", NULL, 8 },
    { "Origin",         "origin",            F_STR,  W_FULL, "An NilBBS system", NULL, 60 },
    { "Inbound",        "inbound",           F_STR,  W_FULL, "BBS:Fido/Inbound", NULL, 200 },
    { "Outbound",       "outbound",          F_STR,  W_FULL, "BBS:Fido/Outbound", NULL, 200 },
    { "Unpack cmd",     "unpack",            F_STR,  W_FULL, "", NULL, 200 },
    { NULL }
};
/* the generated menu's colours (menu.c): accent = the hot keys + title tab (0-7, the |16-|23
 * background order), frame = the box (0-15).  Saved as "4 red" - menu.c reads the number. */
static const char *cyc_accent[] = { "0 black", "1 blue", "2 green", "3 cyan", "4 red", "5 magenta",
                                    "6 brown", "7 grey", NULL };
static const char *cyc_frame[]  = { "0 black", "1 blue", "2 green", "3 cyan", "4 red", "5 magenta",
                                    "6 brown", "7 grey", "8 dark grey", "9 lt blue", "10 lt green",
                                    "11 lt cyan", "12 lt red", "13 lt magenta", "14 yellow", "15 white", NULL };
static struct Field f_menuhdr[] = {
    { "Title",          "title",             F_STR,  W_FULL, "", NULL, 47 },
    { "Prompt",         "prompt",            F_STR,  W_FULL, "", NULL, 150 },
    /* full-width rows: beside the menu list a half or third field leaves the gadget ~4 chars */
    { "Screen",         "screen",            F_STR,  W_FULL, "", NULL, 31 },   /* replaces the item list */
    { "Header",         "header",            F_STR,  W_FULL, "", NULL, 31 },   /* drawn above it */
    { "Accent",         "accent",            F_CYC,  W_FULL, "4 red", cyc_accent, 0 },
    { "Frame",          "frame",             F_CYC,  W_FULL, "8 dark grey", cyc_frame, 0 },
    { "Menu level",     "level",             F_INT,  W_HALF, "0", NULL, 3 },
    { NULL }
};
#define MENUHDR_ROWS 7                      /* the rows f_menuhdr takes (the menu list matches) */
/* menu items are pipe-separated; these fields are addressed by column */
static struct Field f_menuitem[] = {
    { "Key",            "0",                 F_STR,  W_THIRD, "", NULL, 1 },
    { "Level",          "1",                 F_INT,  W_THIRD, "0", NULL, 3 },
    { "Command",        "2",                 F_STR,  W_THIRD, "", NULL, 15 },
    { "Argument",       "3",                 F_STR,  W_HALF, "", NULL, 63 },
    { "Description",    "4",                 F_STR,  W_HALF, "", NULL, 47 },
    { NULL }
};
static struct Field f_user[] = {
    { "Level",          "level",             F_INT,  W_HALF, "10", NULL, 3 },
    { "New password",   "pass",              F_STR,  W_HALF, "", NULL, 30 },
    { "Real name",      "realname",          F_STR,  W_HALF, "", NULL, 47 },
    { "Location",       "location",          F_STR,  W_HALF, "", NULL, 47 },
    { "E-mail",         "email",             F_STR,  W_FULL, "", NULL, 63 },
    { "Flags A-Z",      "aflags",            F_STR,  W_HALF, "", NULL, 26 },
    { "Group",          "group",             F_STR,  W_HALF, "", NULL, 15 },
    { "Credits (KB)",   "credits",           F_INT,  W_HALF, "0", NULL, 8 },
    { "Awaiting OK",    "newuser",           F_BOOL, W_HALF, "no", NULL, 0 },
    { "Locked",         "locked",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Deleted",        "deleted",           F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Hidden",         "hidden",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { NULL }
};
static const char *cyc_uterm[]  = { "ASCII", "ANSI", "VT100", NULL };            /* TT_* order */
static const char *cyc_ucs[]    = { "CP437", "UTF-8", "Latin-1", "ASCII", NULL }; /* CS_* order */
static const char *cyc_uproto[] = { "ZMODEM", "YMODEM", "XMODEM-1K", "XMODEM", NULL };   /* PROTO_* order */
static struct Field f_user_set[] = {
    { "Terminal",       "termtype",          F_CYC,  W_HALF, "ANSI", cyc_uterm, 0 },
    { "Charset",        "charset",           F_CYC,  W_HALF, "CP437", cyc_ucs, 0 },
    { "Columns",        "cols",              F_INT,  W_HALF, "80", NULL, 3 },
    { "Rows",           "rows",              F_INT,  W_HALF, "24", NULL, 3 },
    { "Protocol",       "proto",             F_CYC,  W_HALF, "ZMODEM", cyc_uproto, 0 },
    { "Conference #",   "conf",              F_INT,  W_HALF, "0", NULL, 3 },
    { "Language",       "lang",              F_STR,  W_HALF, "", NULL, LANG_NAMELEN - 1 },
    { "Hot keys",       "hotkeys",           F_BOOL, W_THIRD, "yes", NULL, 0 },
    { "Expert",         "expert",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { "No pages",       "nopage",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Line editor",    "lineedit",          F_BOOL, W_THIRD, "no", NULL, 0 },
    { "Term set",       "termset",           F_BOOL, W_THIRD, "no", NULL, 0 },
    { "More off",       "nomore",            F_BOOL, W_THIRD, "no", NULL, 0 },
    { NULL }
};
static struct Field f_user_cnt[] = {
    { "Calls",          "calls",             F_INT,  W_HALF, "0", NULL, 8 },
    { "Posts",          "posts",             F_INT,  W_HALF, "0", NULL, 8 },
    { "Uploads",        "uploads",           F_INT,  W_HALF, "0", NULL, 8 },
    { "Downloads",      "downloads",         F_INT,  W_HALF, "0", NULL, 8 },
    { "Upload KB",      "ulkb",              F_INT,  W_HALF, "0", NULL, 9 },
    { "Download KB",    "dlkb",              F_INT,  W_HALF, "0", NULL, 9 },
    { "Mins today",     "mins_today",        F_INT,  W_HALF, "0", NULL, 5 },
    { "Calls today",    "calls_today",       F_INT,  W_HALF, "0", NULL, 5 },
    { "Door runs",      "doors",             F_INT,  W_HALF, "0", NULL, 8 },
    { "Clear last-read","clearlr",           F_BOOL, W_HALF, "no", NULL, 0 },
    { NULL }
};
static struct Field *user_pages[] = { f_user, f_user_set, f_user_cnt };
static const char *cyc_upage[] = { "Account", "Settings", "Counters", NULL };
static int upage;
/* Doors: which page, and the Show labels (Hang-up only for the types that use it) */
static const char *cyc_dpage3[] = { "Door", "Options", "Hang-up", NULL };
static const char *cyc_dpage2[] = { "Door", "Options", NULL };
static int dpage;
static int door_keep = -1;                  /* the door selected across a Doors page rebuild */
static BOOL panel_nocommit;                 /* close_panel: don't write the gadgets back this once */

/* ---- panels ------------------------------------------------------------------ */

#define PK_SIMPLE 0     /* key = value, top level of one file */
#define PK_LIST   1     /* one [SECTION] per item */
#define PK_IP     2     /* IPFilter.cfg rules */
#define PK_MENU   3     /* Menus/<name>.mnu */
#define PK_USERS  4     /* Data/Users.dat */
#define PK_NAMES  5     /* one entry per line: BannedNames.cfg */
#define PK_CD     6     /* FileAreas.cfg, only the CD/DVD areas, + the drives on this Amiga */
#define IS_LIST(k) ((k) == PK_LIST || (k) == PK_CD)

enum { P_SYSTEM, P_NEWUSERS, P_SECURITY, P_IP, P_NAMES, P_STRIP, P_DOORS, P_MSG, P_FILE, P_CD, P_FILES, P_CONFS, P_BULL, P_EVENTS,
       P_MAINT, P_COMMUNITY, P_FIDO, P_MENUS, P_USERS, NPANELS };

static struct Ini *ini_main, *ini_ip, *ini_doors, *ini_msg, *ini_file, *ini_fido, *ini_menu;
static struct Ini *ini_conf, *ini_bull, *ini_ev, *ini_names, *ini_strip;

struct Panel {
    const char *name;
    UBYTE kind;
    struct Ini **ini;
    struct Field *fields;
    const char *hint;
};
static struct Panel panels[NPANELS] = {
    { "System",           PK_SIMPLE, &ini_main,  f_system,    "Port and nodes take effect when NilBBS restarts." },
    { "New users", PK_SIMPLE, &ini_main,  f_newusers,  "Validated lvl 0 = no validation.  Min by level: level:minutes, 0 = unlimited." },
    { "Security",         PK_SIMPLE, &ini_main,  f_security,  "\\r\\n in messages = new line." },
    { "IP rules",         PK_IP,     &ini_ip,    NULL,        "allow a.b.c.d/nn  or  deny 1.2.*  (allow = whitelist)" },
    { "Banned names",     PK_NAMES,  &ini_names, NULL,        "Nobody may sign up as these.  * = anything (*sysop*)" },
    { "Stripped ads",     PK_NAMES,  &ini_strip, NULL,        "Files taken out of uploaded LHA/LZH archives. #? = anything" },
    { "Doors",            PK_LIST,   &ini_doors, f_doors,     "%u handle %t mins %n node %f drop file.  Detect reads the program, Check tests." },
    { "Message areas",    PK_LIST,   &ini_msg,   f_msgareas,  "Sub-ops moderate. Hold: posts below that level / first N wait." },
    { "File areas",       PK_LIST,   &ini_file,  f_fileareas, "Descriptions in files.bbs. Parent = sub-area of that tag; no path = a heading. CD/DVD drives have their own page." },
    { "CD/DVD drives",    PK_CD,     &ini_file,  f_cdareas,   "One area per drive (or folder on a disc).  Download only." },
    { "Files & DIZ",      PK_SIMPLE, &ini_main,  f_files,     "%a = archive  %d = directory.  House style: Text/diz.tmpl" },
    { "Conferences",      PK_LIST,   &ini_conf,  f_confs,     "Areas join one with conf = TAG.  No sections = no conferences." },
    { "Bulletins",        PK_LIST,   &ini_bull,  f_bulletins, "Screen file is in BBS:Text, without .ans/.asc." },
    { "Events",           PK_LIST,   &ini_ev,    f_events,    "Days: daily or Mon Tue ...  Exclusive logs everyone off." },
    { "Maintenance",      PK_SIMPLE, &ini_main,  f_maint,     "0 = no limit.  When it runs: the NIGHTLY event (Events)." },
    { "Community/QWK",   PK_SIMPLE, &ini_main,  f_community, "ACS: L20 FA Gstaff @name ... | = or.  Finger 0 = off." },
    { "FidoNet",          PK_SIMPLE, &ini_fido,  f_fido,      "Addresses like 21:4/101.  Run BBSToss after mail." },
    { "Menus",            PK_MENU,   &ini_menu,  NULL,        "Commands: menu gosub return door doors msgread ..." },
    { "Users",            PK_USERS,  NULL,       f_user,      "Blank pass = keep.  NEW = awaiting validation." },
};

/* ---- GUI state ---------------------------------------------------------------- */

#define MAXFIELDS 24
#define MAXITEMS  128

static struct Window *win;
static struct Screen *scr;
static APTR vi;
static struct Gadget *sglist, *pglist;          /* static + panel gadgets */
static struct Gadget *g_sections, *g_status;
static struct Gadget *fgad[MAXFIELDS], *fgad2[MAXFIELDS];
static struct Gadget *g_list, *g_list2, *g_rule, *g_upage, *g_drives;
static int cur_panel = -1;
static WORD px, py, pw, ph;                     /* panel area */
static UWORD fh, rowh;
static struct TextAttr *tattr;
static char status[160];

static struct List seclist, itemlist, itemlist2;
static struct Node secnodes[NPANELS];
static struct Node itemnodes[MAXITEMS], itemnodes2[MAXITEMS];
static char itemtext[MAXITEMS][64], itemtext2[MAXITEMS][64];
static char names[MAXITEMS][32];
static int nitems, nitems2, sel = -1, sel2 = -1;
static int itemline[MAXITEMS];                  /* IP rules / menu items: line numbers */
static char menunames[MAXITEMS][32];
static int nmenus, cur_menu = -1;

/* users */
static struct UserRec *users;
static UBYTE *udirty;
static char newpass[MAXITEMS][32];
static int nusers;

static BOOL bbs_running(void) { return shared_find() != NULL; }

static void list_init(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static void set_status(const char *s)
{
    str_copy(status, s, sizeof(status));
    if (g_status) GT_SetGadgetAttrs(g_status, win, NULL, GTTX_Text, (ULONG)status, TAG_END);
}

/* ---- field values: ini <-> gadgets ----------------------------------------------- */

static int cyc_index(const char **cyc, const char *v)
{
    int i;
    for (i = 0; cyc[i]; i++) if (!str_icmp(cyc[i], v)) return i;
    /* numbered labels ("4 red"): a file that holds just the number ("4") picks that entry */
    if (v[0] >= '0' && v[0] <= '9')
        for (i = 0; cyc[i]; i++)
            if (cyc[i][0] >= '0' && cyc[i][0] <= '9' && atoi(cyc[i]) == atoi(v)) return i;
    return 0;
}

/* the text of one menu-item column */
static void menu_col(int line, int col, char *out, int size)
{
    char buf[INI_LINELEN], *f[5], *eq;
    int n;
    out[0] = 0;
    if (line < 0 || line >= ini_menu->n) return;
    str_copy(buf, ini_menu->line[line], sizeof(buf));
    if (!(eq = strchr(buf, '='))) return;
    n = str_split(eq + 1, '|', f, 5);
    if (col < n) str_copy(out, f[col], size);
}

static void menu_setcol(int line, int col, const char *val)
{
    char cols[5][80], buf[INI_LINELEN];
    int i;
    for (i = 0; i < 5; i++) menu_col(line, i, cols[i], sizeof(cols[i]));
    str_copy(cols[col], val, sizeof(cols[col]));
    for (i = 0; i < 5; i++) { char *p; for (p = cols[i]; *p; p++) if (*p == '|') *p = '!'; }
    if (cols[0][0] >= 'a' && cols[0][0] <= 'z') cols[0][0] -= 32;
    sprintf(buf, "item = %s | %s | %s | %s | %s", cols[0], cols[1][0] ? cols[1] : "0", cols[2], cols[3], cols[4]);
    if (strcmp(buf, ini_menu->line[line])) ini_replace_line(ini_menu, line, buf);
}

/* current value of field f for the selected record */
static void field_get(struct Field *f, char *out, int size)
{
    struct Panel *p = &panels[cur_panel];
    out[0] = 0;
    if (p->kind == PK_USERS) {
        struct UserRec *u;
        if (sel < 0 || sel >= nusers) return;
        u = &users[sel];
        if (!strcmp(f->key, "level")) sprintf(out, "%d", (int)u->level);
        else if (!strcmp(f->key, "pass")) str_copy(out, newpass[sel], size);
        else if (!strcmp(f->key, "realname")) str_copy(out, u->realname, size);
        else if (!strcmp(f->key, "location")) str_copy(out, u->location, size);
        else if (!strcmp(f->key, "email")) str_copy(out, u->email, size);
        else if (!strcmp(f->key, "aflags")) {
            int i, n = 0;
            for (i = 0; i < 26 && n < size - 1; i++) if (u->aflags & (1UL << i)) out[n++] = (char)('A' + i);
            out[n] = 0;
        }
        else if (!strcmp(f->key, "group")) str_copy(out, u->group, size);
        else if (!strcmp(f->key, "credits")) sprintf(out, "%ld", u->credits);
        else if (!strcmp(f->key, "locked")) strcpy(out, (u->flags & UF_LOCKED) ? "yes" : "no");
        else if (!strcmp(f->key, "deleted")) strcpy(out, (u->flags & UF_DELETED) ? "yes" : "no");
        else if (!strcmp(f->key, "hidden")) strcpy(out, (u->flags & UF_HIDDEN) ? "yes" : "no");
        else if (!strcmp(f->key, "newuser")) strcpy(out, (u->flags & UF_NEWUSER) ? "yes" : "no");
        else if (!strcmp(f->key, "termtype")) strcpy(out, cyc_uterm[u->termtype <= TT_VT100 ? u->termtype : TT_ANSI]);
        else if (!strcmp(f->key, "charset")) strcpy(out, cyc_ucs[u->charset <= CS_ASCII ? u->charset : CS_CP437]);
        else if (!strcmp(f->key, "proto")) strcpy(out, cyc_uproto[u->proto <= 3 ? u->proto : 0]);
        else if (!strcmp(f->key, "cols")) sprintf(out, "%d", (int)u->cols);
        else if (!strcmp(f->key, "rows")) sprintf(out, "%d", (int)u->rows);
        else if (!strcmp(f->key, "conf")) sprintf(out, "%d", (int)u->conf);
        else if (!strcmp(f->key, "lang")) { str_copy(out, u->lang, size < LANG_NAMELEN ? size : LANG_NAMELEN); }
        else if (!strcmp(f->key, "hotkeys")) strcpy(out, (u->flags & UF_HOTKEYS) ? "yes" : "no");
        else if (!strcmp(f->key, "expert")) strcpy(out, (u->flags & UF_EXPERT) ? "yes" : "no");
        else if (!strcmp(f->key, "nopage")) strcpy(out, (u->flags & UF_NOPAGE) ? "yes" : "no");
        else if (!strcmp(f->key, "lineedit")) strcpy(out, (u->flags & UF_LINEEDIT) ? "yes" : "no");
        else if (!strcmp(f->key, "termset")) strcpy(out, (u->flags & UF_TERMSET) ? "yes" : "no");
        else if (!strcmp(f->key, "nomore")) strcpy(out, (u->flags & UF_NOMORE) ? "yes" : "no");
        else if (!strcmp(f->key, "calls")) sprintf(out, "%lu", u->calls);
        else if (!strcmp(f->key, "posts")) sprintf(out, "%lu", u->posts);
        else if (!strcmp(f->key, "uploads")) sprintf(out, "%lu", u->uploads);
        else if (!strcmp(f->key, "downloads")) sprintf(out, "%lu", u->downloads);
        else if (!strcmp(f->key, "ulkb")) sprintf(out, "%lu", u->ulkb);
        else if (!strcmp(f->key, "dlkb")) sprintf(out, "%lu", u->dlkb);
        else if (!strcmp(f->key, "mins_today")) sprintf(out, "%d", (int)u->mins_today);
        else if (!strcmp(f->key, "calls_today")) sprintf(out, "%d", (int)u->calls_today);
        else if (!strcmp(f->key, "doors")) sprintf(out, "%lu", u->doors);
        else if (!strcmp(f->key, "clearlr")) strcpy(out, "no");      /* an action, not a setting */
        return;
    }
    if (f == &f_menuitem[0] || (f >= f_menuitem && f < f_menuitem + 5)) {
        if (sel2 >= 0) menu_col(itemline[sel2], atoi(f->key), out, size);
        if (!out[0] && f->def) str_copy(out, f->def, size);
        return;
    }
    if (!f->key) {                                  /* the section's tag */
        if (sel >= 0) str_copy(out, names[sel], size);
        return;
    }
    if (IS_LIST(p->kind) && sel < 0) return;
    if (!ini_get(*p->ini, IS_LIST(p->kind) ? names[sel] : NULL, f->key, out, size))
        str_copy(out, f->def ? f->def : "", size);
    else if (cur_panel == P_DOORS && !strcmp(f->key, "type"))
        str_copy(out, dc_type_norm(out), size);         /* arexx / amiexpress: the names we show */
}

static void field_put(struct Field *f, const char *val)
{
    struct Panel *p = &panels[cur_panel];
    char old[INI_LINELEN];
    field_get(f, old, sizeof(old));
    if (!strcmp(old, val)) return;                  /* unchanged: leave the file alone */
    if (cur_panel == P_DOORS && f->key && !strcmp(f->key, "type") && sel >= 0) {
        char raw[24];                               /* "arexx" in the file shows as cnetrexx: keep it */
        if (ini_get(*p->ini, names[sel], "type", raw, sizeof(raw)) && !str_icmp(dc_type_norm(raw), val)) return;
    }

    if (p->kind == PK_USERS) {
        struct UserRec *u;
        if (sel < 0 || sel >= nusers) return;
        u = &users[sel];
        if (!strcmp(f->key, "level")) u->level = (UBYTE)atoi(val);
        else if (!strcmp(f->key, "pass")) str_copy(newpass[sel], val, sizeof(newpass[sel]));
        else if (!strcmp(f->key, "realname")) str_copy(u->realname, val, sizeof(u->realname));
        else if (!strcmp(f->key, "lang")) str_copy(u->lang, val, sizeof(u->lang));      /* "" = the board's */
        else if (!strcmp(f->key, "location")) str_copy(u->location, val, sizeof(u->location));
        else if (!strcmp(f->key, "email")) str_copy(u->email, val, sizeof(u->email));
        else if (!strcmp(f->key, "aflags")) {
            const char *p;
            u->aflags = 0;
            for (p = val; *p; p++) {
                char c = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
                if (c >= 'A' && c <= 'Z') u->aflags |= 1UL << (c - 'A');
            }
        }
        else if (!strcmp(f->key, "group")) str_copy(u->group, val, sizeof(u->group));
        else if (!strcmp(f->key, "credits")) u->credits = atol(val);
        else if (!strcmp(f->key, "locked")) u->flags = (u->flags & ~UF_LOCKED) | (val[0] == 'y' ? UF_LOCKED : 0);
        else if (!strcmp(f->key, "deleted")) u->flags = (u->flags & ~UF_DELETED) | (val[0] == 'y' ? UF_DELETED : 0);
        else if (!strcmp(f->key, "hidden")) u->flags = (u->flags & ~UF_HIDDEN) | (val[0] == 'y' ? UF_HIDDEN : 0);
        else if (!strcmp(f->key, "newuser")) u->flags = (u->flags & ~UF_NEWUSER) | (val[0] == 'y' ? UF_NEWUSER : 0);
        else if (!strcmp(f->key, "termtype")) u->termtype = (UBYTE)cyc_index(cyc_uterm, val);
        else if (!strcmp(f->key, "charset")) u->charset = (UBYTE)cyc_index(cyc_ucs, val);
        else if (!strcmp(f->key, "proto")) u->proto = (UBYTE)cyc_index(cyc_uproto, val);
        else if (!strcmp(f->key, "cols")) { int v = atoi(val); u->cols = (UBYTE)(v < 20 ? 20 : v > 255 ? 255 : v); }
        else if (!strcmp(f->key, "rows")) { int v = atoi(val); u->rows = (UBYTE)(v < 10 ? 10 : v > 255 ? 255 : v); }
        else if (!strcmp(f->key, "conf")) u->conf = (UBYTE)atoi(val);
        else if (!strcmp(f->key, "hotkeys")) u->flags = (u->flags & ~UF_HOTKEYS) | (val[0] == 'y' ? UF_HOTKEYS : 0);
        else if (!strcmp(f->key, "expert")) u->flags = (u->flags & ~UF_EXPERT) | (val[0] == 'y' ? UF_EXPERT : 0);
        else if (!strcmp(f->key, "nopage")) u->flags = (u->flags & ~UF_NOPAGE) | (val[0] == 'y' ? UF_NOPAGE : 0);
        else if (!strcmp(f->key, "lineedit")) u->flags = (u->flags & ~UF_LINEEDIT) | (val[0] == 'y' ? UF_LINEEDIT : 0);
        else if (!strcmp(f->key, "termset")) u->flags = (u->flags & ~UF_TERMSET) | (val[0] == 'y' ? UF_TERMSET : 0);
        else if (!strcmp(f->key, "nomore")) u->flags = (u->flags & ~UF_NOMORE) | (val[0] == 'y' ? UF_NOMORE : 0);
        else if (!strcmp(f->key, "calls")) u->calls = strtoul(val, NULL, 10);
        else if (!strcmp(f->key, "posts")) u->posts = strtoul(val, NULL, 10);
        else if (!strcmp(f->key, "uploads")) u->uploads = strtoul(val, NULL, 10);
        else if (!strcmp(f->key, "downloads")) u->downloads = strtoul(val, NULL, 10);
        else if (!strcmp(f->key, "ulkb")) u->ulkb = strtoul(val, NULL, 10);
        else if (!strcmp(f->key, "dlkb")) u->dlkb = strtoul(val, NULL, 10);
        else if (!strcmp(f->key, "mins_today")) u->mins_today = (UWORD)atoi(val);
        else if (!strcmp(f->key, "calls_today")) u->calls_today = (UWORD)atoi(val);
        else if (!strcmp(f->key, "doors")) u->doors = strtoul(val, NULL, 10);
        else if (!strcmp(f->key, "clearlr")) {
            memset(u->lastread, 0, sizeof(u->lastread));
            u->lastfscan = 0;
            set_status("Last-read pointers cleared: every message and file is new to them again.");
        }
        udirty[sel] = 1;
        return;
    }
    if (f >= f_menuitem && f < f_menuitem + 5) {
        if (sel2 >= 0) menu_setcol(itemline[sel2], atoi(f->key), val);
        return;
    }
    if (!f->key) {                                  /* rename the section */
        char tag[32];
        char *q;
        str_copy(tag, val, sizeof(tag));
        for (q = tag; *q; q++) { if (*q >= 'a' && *q <= 'z') *q -= 32; if (*q == ' ' || *q == ']' || *q == '[') *q = '_'; }
        if (!tag[0] || sel < 0) return;
        if (ini_rename_section(*p->ini, names[sel], tag)) str_copy(names[sel], tag, sizeof(names[sel]));
        else set_status("That tag is already used.");
        return;
    }
    if (IS_LIST(p->kind) && sel < 0) return;
    ini_set(*p->ini, IS_LIST(p->kind) ? names[sel] : NULL, f->key, val);
}

/* read every field gadget back into the model */
static void commit_fields(struct Field *fields, struct Gadget **g)
{
    int i;
    if (!fields || !win) return;
    for (i = 0; fields[i].label && i < MAXFIELDS; i++) {
        struct Field *f = &fields[i];
        char val[INI_LINELEN];
        if (!g[i]) continue;
        switch (f->kind) {
        case F_STR:  str_copy(val, (char *)((struct StringInfo *)g[i]->SpecialInfo)->Buffer, sizeof(val)); break;
        case F_INT:  sprintf(val, "%ld", ((struct StringInfo *)g[i]->SpecialInfo)->LongInt); break;
        case F_BOOL: {
            ULONG v = 0;
            GT_GetGadgetAttrs(g[i], win, NULL, GTCB_Checked, (ULONG)&v, TAG_END);
            strcpy(val, v ? "yes" : "no");
            break;
        }
        case F_CYC: {
            ULONG v = 0;
            GT_GetGadgetAttrs(g[i], win, NULL, GTCY_Active, (ULONG)&v, TAG_END);
            strcpy(val, f->cyc[v]);
            break;
        }
        }
        field_put(f, val);
    }
}

static void load_fields(struct Field *fields, struct Gadget **g, BOOL enabled)
{
    int i;
    if (!fields) return;
    for (i = 0; fields[i].label && i < MAXFIELDS; i++) {
        struct Field *f = &fields[i];
        char val[INI_LINELEN];
        if (!g[i]) continue;
        field_get(f, val, sizeof(val));
        switch (f->kind) {
        case F_STR:  GT_SetGadgetAttrs(g[i], win, NULL, GTST_String, (ULONG)val, GA_Disabled, !enabled, TAG_END); break;
        case F_INT:  GT_SetGadgetAttrs(g[i], win, NULL, GTIN_Number, atol(val), GA_Disabled, !enabled, TAG_END); break;
        case F_BOOL: GT_SetGadgetAttrs(g[i], win, NULL, GTCB_Checked,
                         (val[0] == 'y' || val[0] == 'Y' || val[0] == '1' || val[0] == 't'), GA_Disabled, !enabled, TAG_END); break;
        case F_CYC:  GT_SetGadgetAttrs(g[i], win, NULL, GTCY_Active, cyc_index(f->cyc, val), GA_Disabled, !enabled, TAG_END); break;
        }
    }
}

/* ---- gadget layout ----------------------------------------------------------------- */

#define GID_SECTIONS 1
#define GID_SAVE     2
#define GID_REVERT   3
#define GID_QUIT     4
#define GID_STATUS   5
#define GID_LIST     10
#define GID_LIST2    11
#define GID_NEW      12
#define GID_DELETE   13
#define GID_UP       14
#define GID_DOWN     15
#define GID_RULE     16
#define GID_NEW2     17
#define GID_DELETE2  18
#define GID_UP2      19
#define GID_DOWN2    20
#define GID_DEFAULT  21
#define GID_UPAGE    22      /* Users: Account / Settings / Counters */
#define GID_VALIDATE 23      /* Users: validate the selected new user */
#define GID_DRIVES   24      /* CD/DVD: the drives found */
#define GID_USEDRV   25      /* CD/DVD: point the selected area at the selected drive */
#define GID_TESTDRV  26      /* CD/DVD: what's in the drive now */
#define GID_MSCREEN  27      /* Menus: pick the screen file */
#define GID_MMAKE    28      /* Menus: make the menu's ANSI screen from its items */
#define GID_MNOSCR   29      /* Menus: no screen (drawn from the items) */
#define GID_MSTATUS  30      /* Menus: add the Your account status item */
#define GID_DPAGE    31      /* Doors: Door / Options / Hang-up */
#define GID_DDETECT  32      /* Doors: look at the program, suggest the settings */
#define GID_DCHECK   33      /* Doors: will these settings work here? */
#define GID_FIELD    100     /* + index */
#define GID_FIELD2   200

static struct Gadget *make_fields(struct Gadget *g, struct Field *fields, struct Gadget **out,
                                  WORD x, WORD y, WORD w, int gidbase, WORD *ybottom)
{
    struct NewGadget ng;
    WORD lw = 13 * 8, col = 0, cx, cw, step = rowh, gh = fh + 4;    /* lw: the least room a label gets */
    int i, rows = 1;
    /* too many rows for the panel (a 256-line PAL screen, the Doors page): close them up */
    for (i = 0; fields[i].label && i < MAXFIELDS; i++) {
        int span = fields[i].width == W_FULL ? 6 : fields[i].width == W_HALF ? 3 : 2;
        if (col + span > 6) { col = 0; rows++; }
        col += span;
    }
    col = 0;
    if (y + rows * step > py + ph) {
        step = (py + ph - y) / rows;
        if (step < fh + 3) step = fh + 3;
        if (gh > step) gh = step;
    }
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = tattr;
    for (i = 0; fields[i].label && i < MAXFIELDS; i++) {
        struct Field *f = &fields[i];
        int span = f->width == W_FULL ? 6 : f->width == W_HALF ? 3 : 2;
        if (col + span > 6) { col = 0; y += step; }
        cx = x + (w * col) / 6;
        cw = (w * span) / 6;
        ng.ng_GadgetID = gidbase + i;
        ng.ng_GadgetText = (UBYTE *)f->label;
        ng.ng_Flags = PLACETEXT_LEFT;
        ng.ng_TopEdge = y;
        ng.ng_Height = gh;
        if (f->kind == F_BOOL) {
            WORD tl = (strlen(f->label) + 1) * 8 + 4;
            ng.ng_LeftEdge = cx + tl;
            ng.ng_Width = 26;
            ng.ng_Height = gh - 1;
            g = out[i] = CreateGadget(CHECKBOX_KIND, g, &ng, GTCB_Scaled, TRUE, TAG_END);
        } else {
            WORD need = (strlen(f->label) + 1) * 8 + 8;     /* the label + GadTools' gap: never over the field to its left */
            WORD l = (f->width == W_THIRD || need > lw) ? need : lw;
            ng.ng_LeftEdge = cx + l;
            ng.ng_Width = cw - l - 6;
            if (f->kind == F_STR)
                g = out[i] = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, f->maxc ? f->maxc : 64, TAG_END);
            else if (f->kind == F_INT)
                g = out[i] = CreateGadget(INTEGER_KIND, g, &ng, GTIN_MaxChars, f->maxc ? f->maxc : 6, TAG_END);
            else
                g = out[i] = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)f->cyc, TAG_END);
        }
        col += span;
    }
    if (ybottom) *ybottom = y + step;
    return g;
}

static struct Gadget *make_buttons(struct Gadget *g, WORD x, WORD y, int gid0, BOOL updown)
{
    static const char *lab[] = { "New", "Delete", "Up", "Down" };
    struct NewGadget ng;
    int i, n = updown ? 4 : 2;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = tattr;
    ng.ng_TopEdge = y;
    ng.ng_Height = fh + 4;
    ng.ng_Width = 64;
    for (i = 0; i < n; i++) {
        ng.ng_LeftEdge = x + i * 68;
        ng.ng_GadgetText = (UBYTE *)lab[i];
        ng.ng_GadgetID = gid0 + i;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    return g;
}

static struct Gadget *make_listview(struct Gadget *g, struct Gadget **out, WORD x, WORD y, WORD w, WORD h, int gid)
{
    struct NewGadget ng;
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = tattr;
    ng.ng_LeftEdge = x; ng.ng_TopEdge = y; ng.ng_Width = w; ng.ng_Height = h;
    ng.ng_GadgetID = gid;
    return *out = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_ShowSelected, 0UL, TAG_END);
}

/* ---- list contents ------------------------------------------------------------------- */

/* the lines a line-list page shows: IP rules, or every non-comment line of BannedNames.cfg */
static BOOL line_item(int kind, const char *t)
{
    if (kind == PK_IP) return !str_nicmp(t, "allow", 5) || !str_nicmp(t, "deny", 4);
    return t[0] && t[0] != '#' && t[0] != ';';
}

static void fill_items(void)
{
    struct Panel *p = &panels[cur_panel];
    int i;
    nitems = 0;
    list_init(&itemlist);
    if (p->kind == PK_LIST) {
        int n = ini_sections(*p->ini, names, MAXITEMS);
        for (i = 0; i < n; i++) {
            char nm[48];
            ini_get(*p->ini, names[i], "name", nm, sizeof(nm));
            sprintf(itemtext[i], "%-12.12s %s", names[i], nm);
        }
        nitems = n;
    } else if (p->kind == PK_CD) {
        static char all[MAXITEMS][32];
        int n = ini_sections(*p->ini, all, MAXITEMS);
        for (i = 0; i < n; i++) {
            char nm[48], pa[64], cd[8], ro[8];
            ini_get(*p->ini, all[i], "cdrom", cd, sizeof(cd));
            ini_get(*p->ini, all[i], "readonly", ro, sizeof(ro));
            if (!(cd[0] == 'y' || cd[0] == 'Y' || cd[0] == '1' || ro[0] == 'y' || ro[0] == 'Y' || ro[0] == '1')) continue;
            ini_get(*p->ini, all[i], "name", nm, sizeof(nm));
            ini_get(*p->ini, all[i], "path", pa, sizeof(pa));
            str_copy(names[nitems], all[i], sizeof(names[0]));
            sprintf(itemtext[nitems], "%-10.10s %-9.9s %s", all[i], pa, nm);
            nitems++;
        }
    } else if (p->kind == PK_IP || p->kind == PK_NAMES) {
        for (i = 0; i < (*p->ini)->n && nitems < MAXITEMS; i++) {
            char b[INI_LINELEN], *t;
            str_copy(b, (*p->ini)->line[i], sizeof(b));
            t = str_trim(b);
            if (line_item(p->kind, t)) {
                str_copy(itemtext[nitems], t, 64);
                itemline[nitems++] = i;
            }
        }
    } else if (p->kind == PK_MENU) {
        for (i = 0; i < nmenus; i++) str_copy(itemtext[i], menunames[i], 64);
        nitems = nmenus;
    } else if (p->kind == PK_USERS) {
        for (i = 0; i < nusers && i < MAXITEMS; i++)
            sprintf(itemtext[i], "%-20.20s %3d%s", users[i].name, (int)users[i].level,
                    (users[i].flags & UF_DELETED) ? " del" : (users[i].flags & UF_LOCKED) ? " lck" :
                    (users[i].flags & UF_NEWUSER) ? " NEW - awaiting validation" : "");
        nitems = nusers < MAXITEMS ? nusers : MAXITEMS;
    }
    for (i = 0; i < nitems; i++) {
        itemnodes[i].ln_Name = itemtext[i];
        AddTail(&itemlist, &itemnodes[i]);
    }
    if (sel >= nitems) sel = nitems - 1;
}

static void fill_items2(void)     /* menu items of the current menu */
{
    int i;
    nitems2 = 0;
    list_init(&itemlist2);
    if (cur_menu >= 0 && ini_menu) {
        for (i = 0; i < ini_menu->n && nitems2 < MAXITEMS; i++) {
            char b[INI_LINELEN], *t = b;
            str_copy(b, ini_menu->line[i], sizeof(b));
            t = str_trim(b);
            if (!str_nicmp(t, "item", 4) && strchr(t, '=')) {
                char k[4], lv[8], c[20], d[48];
                itemline[nitems2] = i;
                menu_col(i, 0, k, sizeof(k)); menu_col(i, 1, lv, sizeof(lv));
                menu_col(i, 2, c, sizeof(c)); menu_col(i, 4, d, sizeof(d));
                sprintf(itemtext2[nitems2], "%-2s %3s %-10.10s %s", k, lv, c, d);
                nitems2++;
            }
        }
    }
    for (i = 0; i < nitems2; i++) {
        itemnodes2[i].ln_Name = itemtext2[i];
        AddTail(&itemlist2, &itemnodes2[i]);
    }
    if (sel2 >= nitems2) sel2 = nitems2 - 1;
}

static void refresh_lists(void)
{
    if (g_list) {
        GT_SetGadgetAttrs(g_list, win, NULL, GTLV_Labels, ~0UL, TAG_END);
        fill_items();
        GT_SetGadgetAttrs(g_list, win, NULL, GTLV_Labels, (ULONG)&itemlist,
                          GTLV_Selected, sel >= 0 ? (ULONG)sel : ~0UL,
                          GTLV_MakeVisible, sel >= 0 ? (ULONG)sel : 0UL, TAG_END);
    }
    if (g_list2) {
        GT_SetGadgetAttrs(g_list2, win, NULL, GTLV_Labels, ~0UL, TAG_END);
        fill_items2();
        GT_SetGadgetAttrs(g_list2, win, NULL, GTLV_Labels, (ULONG)&itemlist2,
                          GTLV_Selected, sel2 >= 0 ? (ULONG)sel2 : ~0UL, TAG_END);
    }
}

/* menus: load the chosen .mnu into ini_menu */
static void open_menu(int i)
{
    char path[64];
    if (ini_menu && ini_menu->dirty) ini_save(ini_menu);
    if (ini_menu) { ini_free(ini_menu); ini_menu = NULL; }
    cur_menu = i;
    sel2 = -1;
    if (i < 0) return;
    sprintf(path, "BBS:Menus/%s.mnu", menunames[i]);
    ini_menu = ini_load(path);
}

static void scan_menus(void)
{
    struct AnchorPath *ap = AllocVec(sizeof(struct AnchorPath) + 256, MEMF_CLEAR);
    LONG err;
    nmenus = 0;
    if (!ap) return;
    for (err = MatchFirst((STRPTR)"BBS:Menus/#?.mnu", ap); !err && nmenus < MAXITEMS; err = MatchNext(ap)) {
        char *dot;
        str_copy(menunames[nmenus], (char *)ap->ap_Info.fib_FileName, 32);
        if ((dot = strrchr(menunames[nmenus], '.'))) *dot = 0;
        nmenus++;
    }
    MatchEnd(ap);
    FreeVec(ap);
}

/* ---- users ----------------------------------------------------------------------------- */

static void load_users(void)
{
    struct BBSShared *S = shared_find();
    LONG i, n;
    if (users) { FreeVec(users); FreeVec(udirty); users = NULL; }
    if (S) ObtainSemaphore(&S->userlock);
    n = userdb_count();
    if (n > MAXITEMS) n = MAXITEMS;
    users = AllocVec((n + 1) * sizeof(struct UserRec), MEMF_CLEAR);
    udirty = AllocVec(n + 1, MEMF_CLEAR);
    nusers = 0;
    for (i = 1; users && i <= n; i++)
        if (userdb_read(i, &users[nusers])) nusers++;
    if (S) ReleaseSemaphore(&S->userlock);
    memset(newpass, 0, sizeof(newpass));
}

static int save_users(void)
{
    struct BBSShared *S = shared_find();
    int i, saved = 0;
    if (!users) return 0;
    if (S) ObtainSemaphore(&S->userlock);
    for (i = 0; i < nusers; i++) {
        if (!udirty[i] && !newpass[i][0]) continue;
        if (newpass[i][0]) { user_setpass(&users[i], newpass[i]); newpass[i][0] = 0; }
        if (userdb_write(&users[i])) saved++;
        udirty[i] = 0;
    }
    if (S) ReleaseSemaphore(&S->userlock);
    return saved;
}

/* ---- CD/DVD drives ------------------------------------------------------------------------ */

#define MAXDRV 12
static char drvname[MAXDRV][32];            /* "CD0" */
static char drvtext[MAXDRV][64];            /* "CD0:  UtilityVault" */
static struct List drvlist;
static struct Node drvnodes[MAXDRV];
static int ndrv, dsel = -1;

static void bstr_copy(BSTR b, char *out, int size)
{
    UBYTE *s = (UBYTE *)BADDR(b);
    int n = s ? s[0] : 0;
    if (n >= size) n = size - 1;
    if (s) memcpy(out, s + 1, n);
    out[n] = 0;
}

/* a disc drive: named CD.../DVD..., or run by a CD file system (CDFileSystem, AsimCDFS, cdfs ...) */
static BOOL cd_device(const char *name, const char *handler)
{
    return !str_nicmp(name, "CD", 2) || !str_nicmp(name, "DVD", 3) || !str_nicmp(name, "BD", 2) ||
           str_istr(handler, "cd") || str_istr(handler, "iso9660") || str_istr(handler, "udf");
}

/* what disc is in `path` - no "insert volume" requesters if the drive is empty */
static BOOL disc_in(const char *path, char *vol, int size)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR oldwin = me->pr_WindowPtr;
    BPTR l;
    char buf[256], *c;
    BOOL got = FALSE;
    me->pr_WindowPtr = (APTR)-1;
    if ((l = Lock((STRPTR)path, ACCESS_READ))) {
        buf[0] = 0;
        if (NameFromLock(l, (STRPTR)buf, sizeof(buf)) && (c = strchr(buf, ':'))) *c = 0;
        str_copy(vol, buf, size);
        UnLock(l);
        got = TRUE;
    }
    me->pr_WindowPtr = oldwin;
    return got;
}

static void scan_drives(void)
{
    struct DosList *dl;
    int i;
    ndrv = 0;
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES)) && ndrv < MAXDRV) {
        char nm[32], h[64];
        bstr_copy(dl->dol_Name, nm, sizeof(nm));
        h[0] = 0;
        if (dl->dol_misc.dol_handler.dol_Handler)
            bstr_copy(dl->dol_misc.dol_handler.dol_Handler, h, sizeof(h));
        if (cd_device(nm, h)) str_copy(drvname[ndrv++], nm, sizeof(drvname[0]));
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    list_init(&drvlist);
    for (i = 0; i < ndrv; i++) {                /* after the unlock: DOS calls are allowed again */
        char path[40], vol[40];
        sprintf(path, "%s:", drvname[i]);
        if (disc_in(path, vol, sizeof(vol))) sprintf(drvtext[i], "%-5s %s", path, vol);
        else sprintf(drvtext[i], "%-5s (no disc)", path);
        drvnodes[i].ln_Name = drvtext[i];
        AddTail(&drvlist, &drvnodes[i]);
    }
    if (dsel >= ndrv) dsel = -1;
}

static void cd_new_area(void)
{
    char tag[32], path[40], vol[40], nm[64];
    int k = 1;
    const char *drv = dsel >= 0 ? drvname[dsel] : ndrv ? drvname[0] : "CD0";
    do sprintf(tag, "DISC%d", k++); while (!ini_add_section(ini_file, tag) && k < 100);
    sprintf(path, "%s:", drv);
    if (disc_in(path, vol, sizeof(vol))) sprintf(nm, "CD-ROM: %s", vol);
    else sprintf(nm, "CD-ROM drive %s", path);
    ini_set(ini_file, tag, "name", nm);
    ini_set(ini_file, tag, "path", path);
    ini_set(ini_file, tag, "cdrom", "yes");
    ini_set(ini_file, tag, "readonly", "yes");
    ini_set(ini_file, tag, "download", "10");
    ini_set(ini_file, tag, "upload", "255");
    fill_items();
    for (sel = 0; sel < nitems && str_icmp(names[sel], tag); sel++) ;
    sprintf(status, "Added %s for %s - Save to put it on the BBS.", tag, path);
    set_status(status);
}

static void cd_use_drive(void)
{
    char path[40];
    commit_fields(panels[cur_panel].fields, fgad);
    if (sel < 0) { set_status("Pick an area on the left first."); return; }
    if (dsel < 0) { set_status("Pick a drive on the right first."); return; }
    sprintf(path, "%s:", drvname[dsel]);
    ini_set(ini_file, names[sel], "path", path);
    sprintf(status, "%s now reads %s", names[sel], path);
    set_status(status);
}

static void cd_test(void)
{
    char path[200], vol[40];
    commit_fields(panels[cur_panel].fields, fgad);
    if (sel >= 0) ini_get(ini_file, names[sel], "path", path, sizeof(path));
    else if (dsel >= 0) sprintf(path, "%s:", drvname[dsel]);
    else { set_status("Pick an area or a drive."); return; }
    if (!path[0]) { set_status("That area has no drive/path."); return; }
    if (disc_in(path, vol, sizeof(vol))) sprintf(status, "%s holds \"%s\" - callers can browse it.", path, vol);
    else sprintf(status, "%s is empty (no disc, or no such drive/folder).", path);
    set_status(status);
    scan_drives();
    if (g_drives) {
        GT_SetGadgetAttrs(g_drives, win, NULL, GTLV_Labels, ~0UL, TAG_END);
        GT_SetGadgetAttrs(g_drives, win, NULL, GTLV_Labels, (ULONG)&drvlist,
                          GTLV_Selected, dsel >= 0 ? (ULONG)dsel : ~0UL, TAG_END);
    }
}

/* ---- panel build ---------------------------------------------------------------------- */

static void close_panel(void)
{
    struct Panel *p;
    if (cur_panel < 0) return;
    p = &panels[cur_panel];
    if (panel_nocommit) {                   /* the gadgets still show another record (door_page after a pick) */
        panel_nocommit = FALSE;
    } else if (p->kind == PK_MENU) {
        commit_fields(f_menuhdr, fgad);
        commit_fields(f_menuitem, fgad2);
    } else if (p->kind == PK_IP || p->kind == PK_NAMES) {
        if (g_rule && sel >= 0 && sel < nitems) {
            char *t = (char *)((struct StringInfo *)g_rule->SpecialInfo)->Buffer;
            if (strcmp(t, itemtext[sel]) && line_item(p->kind, str_trim(t)))
                ini_replace_line(*p->ini, itemline[sel], str_trim(t));
        }
    } else commit_fields(p->fields, fgad);
    if (pglist) {
        RemoveGList(win, pglist, -1);
        FreeGadgets(pglist);
        pglist = NULL;
    }
    memset(fgad, 0, sizeof(fgad));
    memset(fgad2, 0, sizeof(fgad2));
    g_list = g_list2 = g_rule = g_upage = g_drives = NULL;
    SetAPen(win->RPort, 0);
    RectFill(win->RPort, px - 6, py, px + pw - 1, py + ph - 1);    /* labels can start left of px */
}

static void refresh_panel_values(void)
{
    struct Panel *p = &panels[cur_panel];
    if (p->kind == PK_SIMPLE) load_fields(p->fields, fgad, TRUE);
    else if (IS_LIST(p->kind) || p->kind == PK_USERS) load_fields(p->fields, fgad, sel >= 0);
    else if (p->kind == PK_MENU) {
        load_fields(f_menuhdr, fgad, cur_menu >= 0);
        load_fields(f_menuitem, fgad2, sel2 >= 0);
    } else if (p->kind == PK_IP || p->kind == PK_NAMES) {
        LONG i;
        BOOL deny = FALSE;
        for (i = 0; p->kind == PK_IP && i < ini_ip->n; i++) {
            char b[INI_LINELEN], *t;
            str_copy(b, ini_ip->line[i], sizeof(b));
            t = str_trim(b);
            if (!str_nicmp(t, "default", 7)) deny = str_istr(t, "deny") != NULL;
        }
        if (p->kind == PK_IP && fgad[0]) GT_SetGadgetAttrs(fgad[0], win, NULL, GTCY_Active, deny ? 1 : 0, TAG_END);
        if (g_rule) GT_SetGadgetAttrs(g_rule, win, NULL, GTST_String,
                                      (ULONG)(sel >= 0 ? itemtext[sel] : ""), GA_Disabled, sel < 0, TAG_END);
    }
}

/* ---- Doors: the page for the selected door's type --------------------------------------- */

static void door_sel_type(char *out, int size)
{
    str_copy(out, "cli", size);
    if (sel >= 0 && ini_get(ini_doors, names[sel], "type", out, size)) str_copy(out, dc_type_norm(out), size);
}

static BOOL door_has_hup(const char *t)     /* hangup_* are read by cli and /X doors only */
{
    return !str_icmp(t, "cli") || !str_icmp(t, "xim");
}

static struct Field *door_fields(void)
{
    char t[24];
    door_sel_type(t, sizeof(t));
    if (dpage == 2 && !door_has_hup(t)) dpage = 1;
    if (dpage == 0) return f_doors;
    if (dpage == 2) return f_dhup;
    if (!str_icmp(t, "cnetrexx") || !str_icmp(t, "aim")) return f_dopt_rexx;
    if (!str_icmp(t, "cnetc")) return f_dopt_cnetc;
    if (!str_icmp(t, "xim")) return f_dopt_xim;
    if (!str_icmp(t, "rlogin")) return f_dopt_rlogin;
    if (!str_icmp(t, "telnet") || !str_icmp(t, "tcp")) return f_dopt_net;
    return f_dopt_cli;
}

static const char *cyc_policy[] = { "allow everyone else", "deny everyone else", NULL };

static void open_panel(int n)
{
    struct Panel *p = &panels[n];
    struct Gadget *g;
    struct NewGadget ng;
    WORD y = py + 4, lvh;

    close_panel();
    cur_panel = n;
    sel = -1; sel2 = -1;
    if (n == P_DOORS) {                     /* rebuilt for a door (door_page): its type picks the fields */
        if (door_keep >= 0 && door_keep < nitems) sel = door_keep;
        door_keep = -1;
        p->fields = door_fields();
    }
    g = CreateContext(&pglist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = tattr;

    switch (p->kind) {
    case PK_SIMPLE:
        g = make_fields(g, p->fields, fgad, px, y, pw, GID_FIELD, NULL);
        break;
    case PK_LIST:
    case PK_USERS:
        if (p->kind == PK_USERS && !users) load_users();       /* kept while unsaved edits live */
        lvh = (n == P_DOORS) ? fh * 3 + 4 : fh * 6 + 4;
        g = make_listview(g, &g_list, px, y, pw, lvh, GID_LIST);
        y += lvh + 2;
        if (p->kind == PK_LIST) { g = make_buttons(g, px, y, GID_NEW, TRUE); y += rowh + 2; }
        if (n == P_DOORS) {                 /* Show Door / Options / Hang-up, Detect, Check */
            char t[24];
            door_sel_type(t, sizeof(t));
            ng.ng_LeftEdge = px + 48; ng.ng_TopEdge = y; ng.ng_Width = 120; ng.ng_Height = fh + 4;
            ng.ng_GadgetText = (UBYTE *)"Show"; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GID_DPAGE;
            g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)(door_has_hup(t) ? cyc_dpage3 : cyc_dpage2),
                             GTCY_Active, (ULONG)dpage, TAG_END);
            ng.ng_Flags = 0; ng.ng_Width = 72;
            ng.ng_LeftEdge = px + 180; ng.ng_GadgetText = (UBYTE *)"Detect"; ng.ng_GadgetID = GID_DDETECT;
            g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
            ng.ng_LeftEdge = px + 256; ng.ng_GadgetText = (UBYTE *)"Check"; ng.ng_GadgetID = GID_DCHECK;
            g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
            y += rowh + 2;
        }
        if (p->kind == PK_USERS) {
            ng.ng_LeftEdge = px + 48; ng.ng_TopEdge = y; ng.ng_Width = 120; ng.ng_Height = fh + 4;
            ng.ng_GadgetText = (UBYTE *)"Show"; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GID_UPAGE;
            g = g_upage = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)cyc_upage, GTCY_Active, (ULONG)upage, TAG_END);
            ng.ng_LeftEdge = px + 180; ng.ng_Width = 140; ng.ng_Flags = 0;
            ng.ng_GadgetText = (UBYTE *)"Validate user"; ng.ng_GadgetID = GID_VALIDATE;
            g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
            y += rowh + 2;
        }
        g = make_fields(g, p->fields, fgad, px, y, pw, GID_FIELD, NULL);
        break;
    case PK_CD: {
        WORD lw2 = (pw * 3) / 5, rx = px + lw2 + 6, rw = pw - lw2 - 6;
        scan_drives();
        lvh = fh * 6 + 4;
        g = make_listview(g, &g_list, px, y, lw2, lvh, GID_LIST);
        ng.ng_LeftEdge = rx; ng.ng_TopEdge = y; ng.ng_Width = rw; ng.ng_Height = lvh;
        ng.ng_GadgetText = NULL; ng.ng_GadgetID = GID_DRIVES; ng.ng_Flags = 0;
        g = g_drives = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&drvlist,
                                    GTLV_ShowSelected, 0UL, GTLV_Selected, dsel >= 0 ? (ULONG)dsel : ~0UL, TAG_END);
        y += lvh + 2;
        g = make_buttons(g, px, y, GID_NEW, FALSE);
        ng.ng_TopEdge = y; ng.ng_Height = fh + 4; ng.ng_Width = rw / 2 - 2;
        ng.ng_LeftEdge = rx; ng.ng_GadgetText = (UBYTE *)"Use drive"; ng.ng_GadgetID = GID_USEDRV;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
        ng.ng_LeftEdge = rx + rw / 2 + 2; ng.ng_GadgetText = (UBYTE *)"Test"; ng.ng_GadgetID = GID_TESTDRV;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
        y += rowh + 2;
        g = make_fields(g, p->fields, fgad, px, y, pw, GID_FIELD, NULL);
        break;
    }
    case PK_IP:
    case PK_NAMES:
        if (p->kind == PK_IP) {
            ng.ng_LeftEdge = px + 96; ng.ng_TopEdge = y; ng.ng_Width = 220; ng.ng_Height = fh + 4;
            ng.ng_GadgetText = (UBYTE *)"Default"; ng.ng_Flags = PLACETEXT_LEFT; ng.ng_GadgetID = GID_DEFAULT;
            g = fgad[0] = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)cyc_policy, TAG_END);
            y += rowh + 2;
        }
        lvh = ph - (y - py) - 2 * rowh - 6;
        g = make_listview(g, &g_list, px, y, pw, lvh, GID_LIST);
        y += lvh + 2;
        ng.ng_LeftEdge = px + 96; ng.ng_TopEdge = y; ng.ng_Width = pw - 96; ng.ng_Height = fh + 4;
        ng.ng_GadgetText = (UBYTE *)(p->kind == PK_IP ? "Rule" : "Name"); ng.ng_GadgetID = GID_RULE;
        ng.ng_Flags = PLACETEXT_LEFT;
        g = g_rule = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, 60, TAG_END);
        y += rowh + 2;
        g = make_buttons(g, px, y, GID_NEW, TRUE);
        break;
    case PK_MENU: {
        WORD half = pw / 3;
        scan_menus();
        lvh = rowh * MENUHDR_ROWS - 2;      /* beside the header fields */
        g = make_listview(g, &g_list, px, y, half - 4, lvh, GID_LIST);
        {
            WORD yb;
            g = make_fields(g, f_menuhdr, fgad, px + half, y, pw - half, GID_FIELD, &yb);
        }
        y += lvh + 2;
        g = make_buttons(g, px, y, GID_NEW, FALSE);
        {   /* after New / Delete: the menu's ANSI screen */
            static const char *lab[] = { "Screen...", "Make screen", "No screen" };
            static const WORD wid[] = { 84, 100, 84 };
            struct NewGadget bg;
            WORD x = px + 2 * 68 + 8;
            int i;
            memset(&bg, 0, sizeof(bg));
            bg.ng_VisualInfo = vi; bg.ng_TextAttr = tattr; bg.ng_TopEdge = y; bg.ng_Height = fh + 4;
            for (i = 0; i < 3; i++) {
                bg.ng_LeftEdge = x; bg.ng_Width = wid[i]; bg.ng_GadgetText = (UBYTE *)lab[i];
                bg.ng_GadgetID = GID_MSCREEN + i;
                g = CreateGadget(BUTTON_KIND, g, &bg, TAG_END);
                x += wid[i] + 4;
            }
        }
        y += rowh + 2;
        lvh = fh * 4 + 4;                   /* the items (it scrolls) - room for the 7 header rows */
        g = make_listview(g, &g_list2, px, y, pw, lvh, GID_LIST2);
        y += lvh + 2;
        g = make_buttons(g, px, y, GID_NEW2, TRUE);
        {   /* after the items' buttons: the account status item (new in 1.3) */
            struct NewGadget bg;
            memset(&bg, 0, sizeof(bg));
            bg.ng_VisualInfo = vi; bg.ng_TextAttr = tattr; bg.ng_TopEdge = y; bg.ng_Height = fh + 4;
            bg.ng_LeftEdge = px + 4 * 68 + 8; bg.ng_Width = 136;
            bg.ng_GadgetText = (UBYTE *)"Add status item"; bg.ng_GadgetID = GID_MSTATUS;
            g = CreateGadget(BUTTON_KIND, g, &bg, TAG_END);
        }
        y += rowh + 2;
        g = make_fields(g, f_menuitem, fgad2, px, y, pw, GID_FIELD2, NULL);
        break;
    }
    }
    if (!g) { set_status("Could not create gadgets (window too small?)"); return; }
    AddGList(win, pglist, -1, -1, NULL);
    RefreshGList(pglist, win, NULL, -1);
    GT_RefreshWindow(win, NULL);
    refresh_lists();
    refresh_panel_values();
    set_status(p->hint);
}

/* ---- actions ---------------------------------------------------------------------------- */

static BOOL ask(const char *text, const char *buttons)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"BBSConfig",
                             (UBYTE *)text, (UBYTE *)buttons };
    return EasyRequestArgs(win, &es, NULL, NULL) == 1;
}
/* ---- a menu's ANSI screen (the Menus page: Make screen / Screen... / No screen / Add status item) ----
 * Make screen draws what BBSNode draws for the menu (menu.c menu_draw at 80 columns: the header banner,
 * then the items in a box with the hot keys as coloured "pills") into BBS:Text/<menu>menu.ans and a
 * plain .asc, and sets "screen = <menu>menu".  The sysop can then redraw it in any ANSI editor.
 * Items a regular caller can't use (level above 10, or an ACS) stay off the picture; they still work.
 * tools/mkscreens.py does the same on a PC. */

#define MS_W 75                             /* inside the box at 80 columns (cols - 5) */
static const UBYTE pc2ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };  /* pipe codes count in PC order (1 blue, 4 red) */

static void ms_put(BPTR fh, const char *s) { FPuts(fh, (STRPTR)s); }

static void ms_sgr(BPTR fh, BOOL ansi, int fg, int bg)
{
    char b[24];
    if (!ansi) return;
    sprintf(b, "\x1b[0;%s%d;%dm", fg >= 8 ? "1;" : "", 30 + pc2ansi[fg & 7], 40 + pc2ansi[bg & 7]);
    ms_put(fh, b);
}

static void ms_rep(BPTR fh, const char *glyph, int n) { while (n-- > 0) ms_put(fh, glyph); }

/* a key or the title in the accent colour: half blocks round it (ANSI), [K] / [ title ] (plain) */
static void ms_tab(BPTR fh, BOOL ansi, int accent, const char *s, BOOL wide)
{
    if (ansi) {
        ms_sgr(fh, TRUE, accent, 0); ms_put(fh, "\xDE");
        ms_sgr(fh, TRUE, 15, accent);
        if (wide) ms_put(fh, " ");
        ms_put(fh, s);
        if (wide) ms_put(fh, " ");
        ms_sgr(fh, TRUE, accent, 0); ms_put(fh, "\xDD");
    } else {
        ms_put(fh, wide ? "[ " : "[");
        ms_put(fh, s);
        ms_put(fh, wide ? " ]" : "]");
    }
}

/* the banner: Text/<name>.ans (or .asc) as it is, up to a DOS EOF / SAUCE; TRUE if there was one */
static BOOL ms_banner(BPTR out, BOOL ansi, const char *name)
{
    static const char *ea[] = { ".ans", ".asc", ".txt", NULL }, *ep[] = { ".asc", ".txt", ".ans", NULL };
    const char **e;
    char path[PATHLEN];
    UBYTE buf[512];
    for (e = ansi ? ea : ep; *e; e++) {
        BPTR in;
        LONG n;
        sprintf(path, "BBS:Text/%s%s", name, *e);
        if (!(in = Open((STRPTR)path, MODE_OLDFILE))) continue;
        while ((n = Read(in, buf, sizeof(buf))) > 0) {
            UBYTE *eof = memchr(buf, 0x1A, n);
            Write(out, buf, eof ? eof - buf : n);
            if (eof) break;
        }
        Close(in);
        return TRUE;
    }
    return FALSE;
}

static BOOL ms_write(const char *path, BOOL ansi, const char *title, const char *header, int accent, int frame)
{
    const char *H = ansi ? "\xC4" : "-", *V = ansi ? "\xB3" : "|";
    const char *nl = ansi ? "\r\n" : "\n";
    int cw = MS_W / 2, dw = cw - 5, tlen, left, right, col = 0, i;
    BOOL banner;
    BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
    if (!fh) return FALSE;
    ms_put(fh, nl);
    banner = header[0] && ms_banner(fh, ansi, header);
    if (banner) ms_put(fh, nl);
    tlen = !banner && title[0] ? (int)strlen(title) + 4 : 0;
    if (tlen > MS_W - 2) tlen = 0;
    left = (MS_W + 1 - tlen) / 2;
    right = MS_W + 1 - tlen - left;
    ms_put(fh, " "); ms_sgr(fh, ansi, frame, 0);
    ms_put(fh, ansi ? "\xDA" : "+"); ms_rep(fh, H, left);
    if (tlen) { ms_tab(fh, ansi, accent, title, TRUE); ms_sgr(fh, ansi, frame, 0); }
    ms_rep(fh, H, right); ms_put(fh, ansi ? "\xBF" : "+");
    if (ansi) ms_put(fh, "\x1b[0m");
    ms_put(fh, nl);
    for (i = 0; i < ini_menu->n; i++) {
        char key[8], lv[48], desc[64];
        const char *p = ini_menu->line[i];
        while (*p == ' ' || *p == '\t') p++;
        if (str_nicmp(p, "item", 4)) continue;
        menu_col(i, 0, key, sizeof(key));
        menu_col(i, 1, lv, sizeof(lv));
        menu_col(i, 4, desc, sizeof(desc));
        if (!key[0] || !desc[0]) continue;
        if (!(lv[0] >= '0' && lv[0] <= '9' && !strchr(lv, ' ') && atoi(lv) <= 10) && lv[0]) continue;  /* not everyone's */
        key[1] = 0;
        if (key[0] >= 'a' && key[0] <= 'z') key[0] -= 32;
        if ((int)strlen(desc) > dw) desc[dw] = 0;
        if (col == 0) { ms_put(fh, " "); ms_sgr(fh, ansi, frame, 0); ms_put(fh, V); ms_put(fh, " "); }
        ms_tab(fh, ansi, accent, key, FALSE);
        ms_put(fh, " "); ms_sgr(fh, ansi, 7, 0); ms_put(fh, desc);
        ms_rep(fh, " ", cw - 4 - (int)strlen(desc));
        if (++col >= 2) {
            ms_rep(fh, " ", MS_W % 2); ms_sgr(fh, ansi, frame, 0); ms_put(fh, V);
            if (ansi) ms_put(fh, "\x1b[0m");
            ms_put(fh, nl);
            col = 0;
        }
    }
    if (col) {
        ms_rep(fh, " ", cw * (2 - col) + MS_W % 2); ms_sgr(fh, ansi, frame, 0); ms_put(fh, V);
        if (ansi) ms_put(fh, "\x1b[0m");
        ms_put(fh, nl);
    }
    ms_put(fh, " "); ms_sgr(fh, ansi, frame, 0);
    ms_put(fh, ansi ? "\xC0" : "+"); ms_rep(fh, H, MS_W + 1); ms_put(fh, ansi ? "\xD9" : "+");
    if (ansi) ms_put(fh, "\x1b[0m");
    ms_put(fh, nl);
    Close(fh);
    return TRUE;
}

static void menu_make_screen(void)
{
    char name[40], title[64], header[40], v[16], ans[PATHLEN], asc[PATHLEN], m[200];
    int accent, frame;
    if (!ini_menu || cur_menu < 0) { set_status("Pick a menu first."); return; }
    commit_fields(f_menuhdr, fgad);
    commit_fields(f_menuitem, fgad2);
    sprintf(name, "%.30smenu", menunames[cur_menu]);
    sprintf(ans, "BBS:Text/%s.ans", name);
    sprintf(asc, "BBS:Text/%s.asc", name);
    if (file_exists(ans) || file_exists(asc)) {
        sprintf(m, "Replace %s.ans / .asc with the menu as it is now?\n(Your own drawing in them would be lost.)", name);
        if (!ask(m, "Replace|Cancel")) return;
    }
    if (!ini_get(ini_menu, NULL, "title", title, sizeof(title))) title[0] = 0;
    if (!ini_get(ini_menu, NULL, "header", header, sizeof(header))) header[0] = 0;
    accent = ini_get(ini_menu, NULL, "accent", v, sizeof(v)) ? atoi(v) & 7 : 4;
    frame = ini_get(ini_menu, NULL, "frame", v, sizeof(v)) ? atoi(v) & 15 : 8;
    if (!ms_write(ans, TRUE, title, header, accent, frame) || !ms_write(asc, FALSE, title, header, accent, frame)) {
        set_status("Could not write the screen into BBS:Text.");
        return;
    }
    ini_set(ini_menu, NULL, "screen", name);
    load_fields(f_menuhdr, fgad, TRUE);
    sprintf(m, "Made Text/%s.ans + .asc and set Screen - Save, then redraw it in any ANSI editor.", name);
    set_status(m);
}

static void menu_pick_screen(void)
{
    struct FileRequester *fr;
    if (!ini_menu || cur_menu < 0) { set_status("Pick a menu first."); return; }
    if (!AslBase) { set_status("No asl.library - type the screen's name in the Screen field."); return; }
    commit_fields(f_menuhdr, fgad);
    if (!(fr = AllocAslRequestTags(ASL_FileRequest, ASLFR_Window, (ULONG)win,
            ASLFR_TitleText, (ULONG)"The menu's screen (in BBS:Text)", ASLFR_InitialDrawer, (ULONG)"BBS:Text",
            ASLFR_InitialPattern, (ULONG)"#?.(ans|asc|txt)", ASLFR_DoPatterns, TRUE, ASLFR_SleepWindow, TRUE,
            TAG_END))) return;
    if (AslRequest(fr, NULL) && fr->fr_File[0]) {
        char name[40], *dot;
        BPTR a = Lock((STRPTR)fr->fr_Drawer, ACCESS_READ), b = Lock((STRPTR)"BBS:Text", ACCESS_READ);
        BOOL same = a && b && SameLock(a, b) == LOCK_SAME;
        if (a) UnLock(a);
        if (b) UnLock(b);
        str_copy(name, (char *)fr->fr_File, sizeof(name));
        if ((dot = strrchr(name, '.'))) *dot = 0;
        if (!same) set_status("A menu screen must be in BBS:Text - copy it there first.");
        else {
            ini_set(ini_menu, NULL, "screen", name);
            load_fields(f_menuhdr, fgad, TRUE);
            set_status("Screen set - Save to use it.");
        }
    }
    FreeAslRequest(fr);
}

static void menu_no_screen(void)
{
    if (!ini_menu || cur_menu < 0) { set_status("Pick a menu first."); return; }
    commit_fields(f_menuhdr, fgad);
    ini_set(ini_menu, NULL, "screen", "");
    load_fields(f_menuhdr, fgad, TRUE);
    set_status("No screen: the menu is drawn from its items again (Save to keep it).");
}

/* the Your account status item (new in 1.3), on a free key - A if it can */
static void menu_add_status(void)
{
    static const char keys[] = "AYZHNQJUVBCDEFGIKLMOPRSTWX";
    char used[40] = "", cmd[20], key[4], line[80];
    int i, last = -1;
    const char *k;
    if (!ini_menu || cur_menu < 0) { set_status("Pick a menu first (usually main)."); return; }
    commit_fields(f_menuitem, fgad2);
    for (i = 0; i < ini_menu->n; i++) {
        const char *p = ini_menu->line[i];
        while (*p == ' ' || *p == '\t') p++;
        if (str_nicmp(p, "item", 4)) continue;
        last = i;
        menu_col(i, 2, cmd, sizeof(cmd));
        if (!str_icmp(cmd, "status")) { set_status("This menu already has the account status item."); return; }
        menu_col(i, 0, key, sizeof(key));
        if (key[0] && strlen(used) < sizeof(used) - 1) {
            char c = key[0] >= 'a' && key[0] <= 'z' ? key[0] - 32 : key[0];
            used[strlen(used) + 1] = 0; used[strlen(used)] = c;
        }
    }
    for (k = keys; *k && strchr(used, *k); k++) ;
    if (!*k) { set_status("No free key left on this menu."); return; }
    sprintf(line, "item = %c | 0 | status |  | Your account status", *k);
    ini_insert_line(ini_menu, last >= 0 ? last + 1 : ini_menu->n, line);
    fill_items2();
    sprintf(line, "Added \"Your account status\" on key %c - Save to keep it.", *k);
    set_status(line);
}


static BOOL any_dirty(void)
{
    int i;
    if (ini_main->dirty || ini_ip->dirty || ini_doors->dirty || ini_msg->dirty ||
        ini_file->dirty || ini_fido->dirty || (ini_menu && ini_menu->dirty) ||
        ini_conf->dirty || ini_bull->dirty || ini_ev->dirty || ini_names->dirty || ini_strip->dirty) return TRUE;
    for (i = 0; users && i < nusers; i++) if (udirty[i] || newpass[i][0]) return TRUE;
    return FALSE;
}

/* a file area whose folder isn't there yet gets one (the whole path, drawer by drawer) -
   so "New" + a path is all it takes to make an area.  Returns how many were made; names go in `made`. */
static int make_area_dirs(char *made, int size)
{
    static char names[64][32], path[256];      /* static: kept off the stack */
    int n, i, count = 0;
    made[0] = 0;
    if (!ini_file) return 0;
    n = ini_sections(ini_file, names, 64);
    for (i = 0; i < n; i++) {
        BPTR l;
        char *p;
        if (!ini_get(ini_file, names[i], "path", path, sizeof(path)) || !path[0]) continue;   /* a heading */
        {
            char ro[8];
            if ((ini_get(ini_file, names[i], "readonly", ro, sizeof(ro)) && (ro[0] == 'y' || ro[0] == 'Y' || ro[0] == '1')) ||
                (ini_get(ini_file, names[i], "cdrom", ro, sizeof(ro)) && (ro[0] == 'y' || ro[0] == 'Y' || ro[0] == '1')))
                continue;                                  /* a CD/DVD: never made, and maybe not in the drive */
        }
        if ((l = Lock((STRPTR)path, ACCESS_READ))) { UnLock(l); continue; }
        for (p = strchr(path, ':') ? strchr(path, ':') + 1 : path; ; p++) {       /* each drawer on the way */
            char c = *p;
            if (c == '/' || c == 0) {
                *p = 0;
                if (!(l = Lock((STRPTR)path, ACCESS_READ))) l = CreateDir((STRPTR)path);
                if (l) UnLock(l);
                *p = c;
            }
            if (!c) break;
        }
        if ((l = Lock((STRPTR)path, ACCESS_READ))) {
            UnLock(l);
            count++;
            if ((int)(strlen(made) + strlen(path) + 3) < size) { strcat(made, "\n  "); strcat(made, path); }
        }
    }
    return count;
}

static void save_all(void)
{
    struct BBSShared *S;
    int files = 0, u;
    struct Ini *all[12];
    int i;
    int keep = cur_panel, keepsel = sel, keepsel2 = sel2, keepmenu = cur_menu;
    if (keep >= 0) {                        /* pull pending gadget edits in */
        close_panel();
        cur_panel = -1;
    }
    all[0] = ini_main; all[1] = ini_ip; all[2] = ini_doors; all[3] = ini_msg;
    all[4] = ini_file; all[5] = ini_fido; all[6] = ini_menu;
    all[7] = ini_conf; all[8] = ini_bull; all[9] = ini_ev; all[10] = ini_names; all[11] = ini_strip;
    for (i = 0; i < 12; i++) {
        if (!all[i] || !all[i]->dirty) continue;
        if (ini_save(all[i])) files++;
        else { char m[300]; sprintf(m, "Could not write %s", all[i]->path); ask(m, "OK"); }
    }
    u = save_users();
    {
        static char made[400], m[480];
        if (make_area_dirs(made, sizeof(made))) {
            sprintf(m, "Made the folders for new file areas:%s", made);
            ask(m, "OK");
        }
    }
    if ((S = shared_find()) && S->daemon) Signal(S->daemon, SIGBREAKF_CTRL_F);
    if (keep >= 0) {
        open_panel(keep);
        if (panels[keep].kind == PK_MENU && keepmenu >= 0) { open_menu(keepmenu); sel = keepmenu; }
        sel = keepsel;
        sel2 = keepsel2;
        refresh_lists();
        refresh_panel_values();
    }
    sprintf(status, "Saved %d file%s%s%s.  New callers get the new settings.", files, files == 1 ? "" : "s",
            u ? " and users" : "", S ? "; NilBBS reloaded its IP filter" : "");
    set_status(status);
}

/* the Language cycle: every .lng file (English is built in), plus the configured one
   even if its file is missing - so saving never quietly switches the board's language */
static void lang_cycle_fill(void)
{
    char cur[LANG_NAMELEN];
    int n = lang_list(lang_nm, MAX_LANGS), i;
    if (!ini_get(ini_main, NULL, "language", cur, sizeof(cur)) || !cur[0]) strcpy(cur, LANG_ENGLISH);
    for (i = 0; i < n && str_icmp(lang_nm[i], cur); i++) ;
    if (i == n) str_copy(lang_nm[n++], cur, LANG_NAMELEN);
    for (i = 0; i < n; i++) cyc_lang[i] = lang_nm[i];
    cyc_lang[n] = NULL;
}

static void load_all(void)
{
    ini_free(ini_main); ini_free(ini_ip); ini_free(ini_doors); ini_free(ini_msg);
    ini_free(ini_file); ini_free(ini_fido); if (ini_menu) ini_free(ini_menu);
    ini_free(ini_conf); ini_free(ini_bull); ini_free(ini_ev); ini_free(ini_names); ini_free(ini_strip);
    ini_main  = ini_load(bbs_config());
    ini_ip    = ini_load("BBS:Config/IPFilter.cfg");
    ini_doors = ini_load("BBS:Config/Doors.cfg");
    ini_msg   = ini_load("BBS:Config/MsgAreas.cfg");
    ini_file  = ini_load("BBS:Config/FileAreas.cfg");
    ini_fido  = ini_load("BBS:Config/Fido.cfg");
    ini_conf  = ini_load("BBS:Config/Conferences.cfg");
    ini_bull  = ini_load("BBS:Config/Bulletins.cfg");
    ini_ev    = ini_load("BBS:Config/Events.cfg");
    ini_names = ini_load("BBS:Config/BannedNames.cfg");
    ini_strip = ini_load("BBS:Config/StripAds.cfg");
    ini_menu  = NULL;
    cur_menu  = -1;
    lang_cycle_fill();
}

static void list_action(int gid)
{
    struct Panel *p = &panels[cur_panel];
    struct Ini *ini = p->ini ? *p->ini : NULL;

    if (p->kind == PK_CD) {
        commit_fields(p->fields, fgad);
        if (gid == GID_NEW) cd_new_area();
        else if (sel >= 0 && gid == GID_DELETE) {
            char m[120];
            sprintf(m, "Take the %s area off the BBS?\n(The disc itself is not touched.)", names[sel]);
            if (ask(m, "Delete|Cancel")) { ini_del_section(ini, names[sel]); sel = -1; }
        }
    } else if (p->kind == PK_LIST) {
        commit_fields(p->fields, fgad);
        if (gid == GID_NEW) {
            char tag[32];
            int k = 1;
            do sprintf(tag, "NEW%d", k++); while (!ini_add_section(ini, tag) && k < 100);
            ini_set(ini, tag, "name", "New entry");
            if (cur_panel == P_DOORS) {             /* a door that is complete enough to Detect/Check */
                ini_set(ini, tag, "type", "cli");
                ini_set(ini, tag, "level", "10");
            }
            fill_items();
            for (sel = 0; sel < nitems && str_icmp(names[sel], tag); sel++) ;
            set_status("Added - give it a tag and fill in the fields.");
        } else if (sel >= 0 && gid == GID_DELETE) {
            char m[120];
            sprintf(m, "Delete %s?", names[sel]);
            if (ask(m, "Delete|Cancel")) { ini_del_section(ini, names[sel]); sel = -1; }
        } else if (sel >= 0 && (gid == GID_UP || gid == GID_DOWN)) {
            char tag[32];
            str_copy(tag, names[sel], sizeof(tag));
            if (ini_move_section(ini, tag, gid == GID_UP ? -1 : 1)) sel += gid == GID_UP ? -1 : 1;
            if (p->fields == f_msgareas) set_status("Note: area order is the area number users' last-read uses.");
        }
    } else if (p->kind == PK_IP || p->kind == PK_NAMES) {
        int last = ini->n;
        if (gid == GID_NEW) {
            int at = nitems ? itemline[nitems - 1] + 1 : last;
            ini_insert_line(ini, at, p->kind == PK_IP ? "deny 0.0.0.0/32" : "newname");
            fill_items();
            sel = nitems - 1;
            set_status(p->kind == PK_IP ? "New rule added - edit it in the Rule field, then press Return."
                                        : "Added - type the name (or a pattern like *sysop*), then press Return.");
        } else if (sel >= 0 && gid == GID_DELETE) {
            ini_delete_line(ini, itemline[sel]);
            sel = -1;
        } else if (sel >= 0 && (gid == GID_UP || gid == GID_DOWN)) {
            int other = sel + (gid == GID_UP ? -1 : 1);
            if (other >= 0 && other < nitems) {
                char a[INI_LINELEN], b[INI_LINELEN];
                str_copy(a, ini->line[itemline[sel]], sizeof(a));
                str_copy(b, ini->line[itemline[other]], sizeof(b));
                ini_replace_line(ini, itemline[sel], b);
                ini_replace_line(ini, itemline[other], a);
                sel = other;
            }
        }
    } else if (p->kind == PK_MENU) {
        commit_fields(f_menuhdr, fgad);
        commit_fields(f_menuitem, fgad2);
        if (gid == GID_MSCREEN) menu_pick_screen();
        else if (gid == GID_MMAKE) menu_make_screen();
        else if (gid == GID_MNOSCR) menu_no_screen();
        else if (gid == GID_MSTATUS) menu_add_status();
        else if (gid == GID_NEW) {
            char path[64];
            int k = 1;
            BPTR fh;
            do sprintf(path, "BBS:Menus/new%d.mnu", k++); while (file_exists(path) && k < 100);
            if ((fh = Open((STRPTR)path, MODE_NEWFILE))) {
                FPuts(fh, (STRPTR)"; NilBBS menu\ntitle  = New Menu\nprompt = |11New Menu|08 > |07\n"
                                  "item = Q | 0 | return |  | Back\n");
                Close(fh);
            }
            scan_menus();
            set_status("Created - rename the .mnu file with the Shell to taste.");
        } else if (cur_menu >= 0 && gid == GID_DELETE) {
            char m[80], path[64];
            sprintf(m, "Delete menu %s?", menunames[cur_menu]);
            if (ask(m, "Delete|Cancel")) {
                sprintf(path, "BBS:Menus/%s.mnu", menunames[cur_menu]);
                if (ini_menu) { ini_menu->dirty = FALSE; }
                open_menu(-1);
                DeleteFile((STRPTR)path);
                scan_menus();
                sel = -1;
            }
        } else if (ini_menu && gid == GID_NEW2) {
            ini_insert_line(ini_menu, ini_menu->n, "item = ? | 0 | text |  | New item");
            fill_items2();
            sel2 = nitems2 - 1;
        } else if (ini_menu && sel2 >= 0 && gid == GID_DELETE2) {
            ini_delete_line(ini_menu, itemline[sel2]);
            sel2 = -1;
        } else if (ini_menu && sel2 >= 0 && (gid == GID_UP2 || gid == GID_DOWN2)) {
            int other = sel2 + (gid == GID_UP2 ? -1 : 1);
            if (other >= 0 && other < nitems2) {
                char a[INI_LINELEN], b[INI_LINELEN];
                str_copy(a, ini_menu->line[itemline[sel2]], sizeof(a));
                str_copy(b, ini_menu->line[itemline[other]], sizeof(b));
                ini_replace_line(ini_menu, itemline[sel2], b);
                ini_replace_line(ini_menu, itemline[other], a);
                sel2 = other;
            }
        }
    }
    refresh_lists();
    refresh_panel_values();
}

/* ---- doors: pages, type defaults, Detect, Check ------------------------------------------- */

/* a requester whose text may hold % (door commands do: %u %t) - it is an argument, not the format */
static BOOL ask_text(const char *text, const char *buttons)
{
    struct EasyStruct es = { sizeof(struct EasyStruct), 0, (UBYTE *)"BBSConfig", (UBYTE *)"%s", (UBYTE *)buttons };
    ULONG arg = (ULONG)text;
    return EasyRequestArgs(win, &es, NULL, &arg) == 1;
}

/* rebuild the Doors panel on page pg for the selected door (its type picks the fields) */
static void door_page(int pg)
{
    int keep = sel;
    close_panel();                          /* commits the page being left */
    dpage = pg;
    door_keep = keep;
    open_panel(P_DOORS);
    refresh_lists();
    refresh_panel_values();
}

/* the type just changed: what that kind of door needs, where we know it.  An Assign that is
 * still the old type's default (set here a moment ago, cycling through types) follows the type. */
static const char *type_assign(const char *t)
{
    if (!str_icmp(t, "cnetrexx") || !str_icmp(t, "cnetc")) return "PFILES: BBS:PFiles";   /* CNet: PFILES: */
    if (!str_icmp(t, "aim") || !str_icmp(t, "xim")) return "DOORS: BBS:Doors";           /* AmiExpress: DOORS: */
    return NULL;
}

static void door_type_defaults(const char *was, const char *t)
{
    char cmd[200], as[100], msg[160];
    const char *want = type_assign(t), *had = type_assign(was);
    if (sel < 0) return;
    if (!ini_get(ini_doors, names[sel], "command", cmd, sizeof(cmd))) cmd[0] = 0;
    if (!ini_get(ini_doors, names[sel], "assign", as, sizeof(as))) as[0] = 0;
    msg[0] = 0;
    if (!as[0] || (had && !str_icmp(as, had))) {        /* no assign, or only the old type's default */
        if (want && (!cmd[0] || !str_nicmp(cmd, want, strchr(want, ':') - want + 1))) {
            ini_set(ini_doors, names[sel], "assign", want);
            sprintf(msg, "%s door: Assign set to %s (change it if the door lives elsewhere).", t, want);
        } else if (as[0]) ini_set(ini_doors, names[sel], "assign", "");
    }
    if (!msg[0]) {
        if (!str_icmp(t, "rlogin") || !str_icmp(t, "telnet") || !str_icmp(t, "tcp"))
            sprintf(msg, "%s door: set Host (and Port) on the Options page.", t);
        else
            sprintf(msg, "%s door: Detect reads its program and fills in the rest.", t);
    }
    set_status(msg);
}

static void door_detect(void)
{
    struct DoorGuess g;
    char cmd[256], dir[200], as[120], cur[24], old[8], drop[24], text[1500], line[200];
    BOOL change = FALSE;
    int k;

    if (sel < 0) { set_status("Pick a door first."); return; }
    commit_fields(panels[P_DOORS].fields, fgad);
    if (!ini_get(ini_doors, names[sel], "command", cmd, sizeof(cmd))) cmd[0] = 0;
    if (!ini_get(ini_doors, names[sel], "dir", dir, sizeof(dir))) dir[0] = 0;
    if (!ini_get(ini_doors, names[sel], "assign", as, sizeof(as))) as[0] = 0;
    door_sel_type(cur, sizeof(cur));
    if (!cmd[0]) { set_status("Fill in Command (and Directory) first - Detect reads that program."); return; }
    set_status("Reading the program...");
    if (!dc_detect(cmd, dir, as, &g)) {
        sprintf(text, "Can't find or read the program for\n  %.200s\n\nCheck Command, Directory and Assign.", cmd);
        ask_text(text, "OK");
        set_status("Detect: program not found.");
        return;
    }
    sprintf(text, "%.200s\nlooks like %s:\n%s.\n", g.program, g.type, g.why);
    strcat(text, "\nSettings:\n");
    if (str_icmp(g.type, cur)) { sprintf(line, "  Type        %s -> %s\n", cur, g.type); strcat(text, line); change = TRUE; }
    if (!ini_get(ini_doors, names[sel], "old_mci", old, sizeof(old))) strcpy(old, "no");
    if (g.oldmci && old[0] != 'y') { strcat(text, "  Old MCI \\c1  yes (CNet 1.x/2.x screen codes)\n"); change = TRUE; }
    if (g.assign[0] && !as[0]) { sprintf(line, "  Assign      %.100s\n", g.assign); strcat(text, line); change = TRUE; }
    else if (g.assign[0] && str_icmp(g.assign, as)) { sprintf(line, "  (its paths suggest  %.100s)\n", g.assign); strcat(text, line); }
    if (g.cnetver == 3) { strcat(text, "  CNet ver    3\n"); change = TRUE; }
    if (g.dropfile[0] && (!ini_get(ini_doors, names[sel], "dropfile", drop, sizeof(drop)) || str_icmp(drop, g.dropfile))) {
        sprintf(line, "  Drop file   %s\n", g.dropfile); strcat(text, line); change = TRUE;
    }
    if (!change) strcat(text, "  nothing to change\n");
    if (g.nlibs) {
        strcat(text, "\nIt uses:");
        for (k = 0; k < g.nlibs; k++) { strcat(text, k ? ", " : " "); strcat(text, g.libs[k]); }
        strcat(text, "\n");
    }
    if (!change) { ask_text(text, "OK"); set_status("Detect: the settings already match."); return; }
    if (!ask_text(text, "Apply|Cancel")) { set_status("Detect: nothing changed."); return; }
    ini_set(ini_doors, names[sel], "type", g.type);
    if (g.oldmci) ini_set(ini_doors, names[sel], "old_mci", "yes");
    if (g.assign[0] && !as[0]) ini_set(ini_doors, names[sel], "assign", g.assign);
    if (g.cnetver == 3) ini_set(ini_doors, names[sel], "cnet_version", "3");
    if (g.dropfile[0]) ini_set(ini_doors, names[sel], "dropfile", g.dropfile);
    panel_nocommit = TRUE;                  /* committed at the start: the file is newer than the gadgets */
    door_page(dpage);
    set_status("Detect: applied - Check tests the result, Save writes Doors.cfg.");
}

static void door_check(void)
{
    char t[24], cmd[256], dir[200], as[120], host[80], rep[1400], text[1500];
    int n;
    if (sel < 0) { set_status("Pick a door first."); return; }
    commit_fields(panels[P_DOORS].fields, fgad);
    door_sel_type(t, sizeof(t));
    if (!ini_get(ini_doors, names[sel], "command", cmd, sizeof(cmd))) cmd[0] = 0;
    if (!ini_get(ini_doors, names[sel], "dir", dir, sizeof(dir))) dir[0] = 0;
    if (!ini_get(ini_doors, names[sel], "assign", as, sizeof(as))) as[0] = 0;
    if (!ini_get(ini_doors, names[sel], "host", host, sizeof(host))) host[0] = 0;
    set_status("Checking...");
    n = dc_check(t, cmd, dir, as, host, rep, sizeof(rep));
    if (!rep[0]) { set_status("Check: nothing wrong found - try the door from a call."); return; }
    sprintf(text, "%s %s:\n\n%s", names[sel], n ? "has problems" : "looks fine, with notes", rep);
    ask_text(text, "OK");
    set_status(n ? "Check: fix the problems listed, then Check again." : "Check: fine (see the notes).");
}

/* ---- users: page switch, validation ------------------------------------------------------- */

static void user_page(int pg)
{
    int keep = sel;
    close_panel();                          /* commits the page being left */
    upage = pg;
    panels[P_USERS].fields = user_pages[pg];
    open_panel(P_USERS);
    sel = keep < nitems ? keep : -1;
    refresh_lists();
    refresh_panel_values();
}

static BOOL user_is_online(const char *name)
{
    struct BBSShared *S = shared_find();
    int i;
    if (!S) return FALSE;
    for (i = 0; i < S->nodes && i < MAX_NODES; i++)
        if (S->node[i].state != NS_FREE && !str_icmp(S->node[i].user, name)) return TRUE;
    return FALSE;
}

static void user_selected(void)
{
    char m[160];
    if (sel < 0 || sel >= nusers) return;
    if (user_is_online(users[sel].name))
        sprintf(m, "%s is online: edit settings/counters after logoff.", users[sel].name);
    else if (users[sel].flags & UF_NEWUSER)
        sprintf(m, "%s awaits validation: Validate user, then Save.", users[sel].name);
    else { set_status(panels[P_USERS].hint); return; }
    set_status(m);
}

static void validate_user(void)
{
    char v[16], m[160];
    LONG vl;
    int i;
    commit_fields(panels[P_USERS].fields, fgad);
    if (sel < 0 || sel >= nusers) { set_status("Pick a user first."); return; }
    ini_get(ini_main, NULL, "validated_level", v, sizeof(v));
    vl = atol(v);
    if (vl > 0 && vl <= 255) users[sel].level = (UBYTE)vl;
    users[sel].flags &= ~UF_NEWUSER;
    udirty[sel] = 1;
    sprintf(m, "Validated %s%s - Save to keep it.", users[sel].name, vl > 0 ? "" : " (level kept)");
    for (i = 1; i <= nusers; i++) {         /* on to the next one waiting */
        int k = (sel + i) % nusers;
        if ((users[k].flags & UF_NEWUSER) && !(users[k].flags & UF_DELETED)) { sel = k; break; }
    }
    refresh_lists();
    refresh_panel_values();
    set_status(m);
}

/* ---- main --------------------------------------------------------------------------------- */

static BOOL ensure_bbs_assign(void)
{
    BPTR l = Lock((STRPTR)"BBS:", ACCESS_READ);
    if (l) { UnLock(l); return TRUE; }
    l = DupLock(GetProgramDir());
    if (!l) return FALSE;
    if (!AssignLock((STRPTR)"BBS", l)) { UnLock(l); return FALSE; }
    return TRUE;
}

static int real_main(void);
/* a GadTools app with deep call chains: don't trust the 4 KB a Shell or icon gives us (2026-09-25: a
   stack overflow here hung the whole bench) */
int main(void) { return run_with_stack(32768, real_main); }

static int real_main(void)
{
    struct NewGadget ng;
    struct Gadget *g;
    WORD ww, wh, lw = 136, bx;
    BOOL done = FALSE;
    int i;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39);
    GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 39);
    AslBase = OpenLibrary((STRPTR)"asl.library", 38);
    GfxBase = (struct GfxBase *)OpenLibrary((STRPTR)"graphics.library", 39);
    if (!IntuitionBase || !GadToolsBase || !GfxBase) {
        PutStr((STRPTR)"BBSConfig needs AmigaOS 3.0 or newer.\n");
        goto out;
    }
    if (!ensure_bbs_assign()) { PutStr((STRPTR)"BBSConfig: can't find BBS:\n"); goto out; }
    load_all();

    if (!(scr = LockPubScreen(NULL))) goto out;
    vi = GetVisualInfo(scr, TAG_END);
    tattr = scr->Font;
    fh = tattr->ta_YSize;
    rowh = fh + 6;
    ww = scr->Width > 640 ? 640 : scr->Width;
    wh = scr->Height - scr->BarHeight - 1;
    if (wh > 400) wh = 400;

    /* static gadgets: section list + bottom buttons + status */
    list_init(&seclist);
    for (i = 0; i < NPANELS; i++) { secnodes[i].ln_Name = (char *)panels[i].name; AddTail(&seclist, &secnodes[i]); }
    g = CreateContext(&sglist);
    memset(&ng, 0, sizeof(ng));
    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = tattr;
    {
        WORD top = scr->WBorTop + fh + 1 + 4, left = scr->WBorLeft + 6;
        WORD inner_h = wh - (scr->WBorTop + fh + 1) - scr->WBorBottom;
        WORD bottom = scr->WBorTop + fh + 1 + inner_h;
        ng.ng_LeftEdge = left; ng.ng_TopEdge = top;
        ng.ng_Width = lw; ng.ng_Height = inner_h - rowh - 12;
        ng.ng_GadgetID = GID_SECTIONS;
        g = g_sections = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, (ULONG)&seclist,
                                      GTLV_ShowSelected, 0UL, GTLV_Selected, 0UL, TAG_END);
        px = left + lw + 8;
        py = top;
        pw = ww - px - scr->WBorRight - 8;
        ph = inner_h - rowh - 12;

        ng.ng_TopEdge = bottom - rowh - 2;
        ng.ng_Height = fh + 5;
        bx = ww - scr->WBorRight - 6 - 3 * 76;
        ng.ng_LeftEdge = left; ng.ng_Width = bx - left - 8;
        ng.ng_GadgetID = GID_STATUS;
        g = g_status = CreateGadget(TEXT_KIND, g, &ng, GTTX_Border, TRUE, GTTX_Text, (ULONG)status, TAG_END);
        ng.ng_Width = 72;
        ng.ng_LeftEdge = bx;       ng.ng_GadgetText = (UBYTE *)"Save";   ng.ng_GadgetID = GID_SAVE;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
        ng.ng_LeftEdge = bx + 76;  ng.ng_GadgetText = (UBYTE *)"Revert"; ng.ng_GadgetID = GID_REVERT;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
        ng.ng_LeftEdge = bx + 152; ng.ng_GadgetText = (UBYTE *)"Quit";   ng.ng_GadgetID = GID_QUIT;
        g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    }
    if (!g) { UnlockPubScreen(NULL, scr); goto out; }

    win = OpenWindowTags(NULL,
        WA_Title, (ULONG)"NilBBS Configuration",
        WA_Left, 0, WA_Top, scr->BarHeight + 1, WA_Width, ww, WA_Height, wh,
        WA_Gadgets, (ULONG)sglist,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_Activate, TRUE,
        WA_PubScreen, (ULONG)scr,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | LISTVIEWIDCMP | BUTTONIDCMP |
                  STRINGIDCMP | CHECKBOXIDCMP | CYCLEIDCMP | INTEGERIDCMP,
        TAG_END);
    UnlockPubScreen(NULL, scr);
    if (!win) goto out;
    GT_RefreshWindow(win, NULL);
    open_panel(P_SYSTEM);
    if (bbs_running()) set_status("NilBBS is running - saved changes reach new callers.");

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(win->UserPort);
        while ((im = GT_GetIMsg(win->UserPort))) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cls == IDCMP_CLOSEWINDOW) {
                if (cur_panel >= 0) { close_panel(); open_panel(cur_panel); }
                if (!any_dirty() || ask("Quit without saving your changes?", "Quit|Cancel")) done = TRUE;
            } else if (cls == IDCMP_REFRESHWINDOW) {
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
            } else if (cls == IDCMP_GADGETUP) {
                int id = gad->GadgetID;
                struct Panel *p = &panels[cur_panel];
                if (id == GID_SECTIONS) open_panel(code);
                else if (id == GID_SAVE) save_all();
                else if (id == GID_REVERT) {
                    if (!any_dirty() || ask("Throw away unsaved changes?", "Revert|Cancel")) {
                        int keep = cur_panel;
                        close_panel();
                        if (ini_menu) ini_menu->dirty = FALSE;
                        cur_panel = -1;
                        load_all();
                        if (users) { FreeVec(users); FreeVec(udirty); users = NULL; udirty = NULL; }
                        memset(newpass, 0, sizeof(newpass));
                        open_panel(keep);
                        set_status("Reloaded from disk.");
                    }
                } else if (id == GID_QUIT) {
                    close_panel(); open_panel(cur_panel);
                    if (!any_dirty() || ask("Quit without saving your changes?", "Quit|Cancel")) done = TRUE;
                } else if (id == GID_LIST) {
                    if (IS_LIST(p->kind) || p->kind == PK_USERS) {
                        commit_fields(p->fields, fgad);
                        refresh_lists();
                        sel = code;
                        refresh_lists();
                        if (p->kind == PK_USERS) user_selected();
                        if (cur_panel == P_DOORS) {     /* its type: its Options, its Show labels */
                            panel_nocommit = TRUE;      /* the gadgets still hold the door left (committed above) */
                            door_page(dpage);
                        }
                    } else if (p->kind == PK_IP || p->kind == PK_NAMES) {
                        if (g_rule && sel >= 0) {
                            char *t = (char *)((struct StringInfo *)g_rule->SpecialInfo)->Buffer;
                            if (strcmp(t, itemtext[sel]) && line_item(p->kind, str_trim(t)))
                                ini_replace_line(*p->ini, itemline[sel], str_trim(t));
                            refresh_lists();
                        }
                        sel = code;
                    } else if (p->kind == PK_MENU) {
                        commit_fields(f_menuhdr, fgad);
                        commit_fields(f_menuitem, fgad2);
                        open_menu(code);
                        sel = code;
                        refresh_lists();
                    }
                    refresh_panel_values();
                } else if (id == GID_LIST2) {
                    commit_fields(f_menuitem, fgad2);
                    refresh_lists();
                    sel2 = code;
                    refresh_panel_values();
                } else if (id == GID_RULE) {
                    if (sel >= 0) {
                        char *t = (char *)((struct StringInfo *)g_rule->SpecialInfo)->Buffer;
                        char *tt = str_trim(t);
                        if (!line_item(p->kind, tt))
                            set_status(p->kind == PK_IP ? "A rule starts with allow or deny."
                                                        : "A name can't be empty or start with # or ;.");
                        else {
                            ini_replace_line(*p->ini, itemline[sel], tt);
                            refresh_lists();
                        }
                    }
                } else if (id == GID_DRIVES) {
                    dsel = code;
                    set_status("Use drive = point the area at it.  New = a new area for it.  Test = what's in it.");
                } else if (id == GID_USEDRV) {
                    cd_use_drive(); refresh_lists(); refresh_panel_values();
                } else if (id == GID_TESTDRV) {
                    cd_test();
                } else if (id == GID_UPAGE) {
                    user_page(code);
                } else if (id == GID_DPAGE) {
                    door_page(code);
                } else if (id == GID_DDETECT) {
                    door_detect();
                } else if (id == GID_DCHECK) {
                    door_check();
                } else if (id == GID_VALIDATE) {
                    validate_user();
                } else if (id == GID_DEFAULT) {
                    int k, found = -1;
                    for (k = 0; k < ini_ip->n; k++) {
                        char b[INI_LINELEN];
                        str_copy(b, ini_ip->line[k], sizeof(b));
                        if (!str_nicmp(str_trim(b), "default", 7)) found = k;
                    }
                    if (found >= 0) ini_replace_line(ini_ip, found, code ? "default deny" : "default allow");
                    else ini_insert_line(ini_ip, 0, code ? "default deny" : "default allow");
                    refresh_lists();
                } else if ((id >= GID_NEW && id <= GID_DOWN2) || (id >= GID_MSCREEN && id <= GID_MSTATUS)) {
                    list_action(id);
                } else if (id >= GID_FIELD) {
                    /* a field changed: apply it now so list labels follow */
                    char before[24] = "", after[24];
                    if (cur_panel == P_DOORS && p->fields == f_doors && id == GID_FIELD + DOOR_TYPE_FIELD)
                        door_sel_type(before, sizeof(before));
                    if (p->kind == PK_MENU) {
                        if (id >= GID_FIELD2) commit_fields(f_menuitem, fgad2);
                        else commit_fields(f_menuhdr, fgad);
                    } else commit_fields(p->fields, fgad);
                    if (p->kind != PK_SIMPLE) refresh_lists();
                    if (before[0]) {                /* a door's type changed: its defaults, its pages */
                        door_sel_type(after, sizeof(after));
                        if (str_icmp(before, after)) {
                            char said[160];
                            door_type_defaults(before, after);
                            str_copy(said, status, sizeof(said));   /* the page rebuild shows its hint */
                            panel_nocommit = TRUE;  /* committed above; the defaults are newer */
                            door_page(0);
                            set_status(said);
                        }
                    }
                }
            }
        }
    }

out:
    if (win) {
        if (pglist) { RemoveGList(win, pglist, -1); FreeGadgets(pglist); }
        CloseWindow(win);
    }
    if (sglist) FreeGadgets(sglist);
    if (vi) FreeVisualInfo(vi);
    if (users) { FreeVec(users); FreeVec(udirty); }
    ini_free(ini_main); ini_free(ini_ip); ini_free(ini_doors); ini_free(ini_msg);
    ini_free(ini_file); ini_free(ini_fido); if (ini_menu) ini_free(ini_menu);
    ini_free(ini_conf); ini_free(ini_bull); ini_free(ini_ev); ini_free(ini_names); ini_free(ini_strip);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (GadToolsBase) CloseLibrary(GadToolsBase);
    if (AslBase) CloseLibrary(AslBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
