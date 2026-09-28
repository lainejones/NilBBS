/*
 * login.c - logon, new-user application, logoff, time accounting.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

/* "0:20,10:60,100:120,255:0" -> minutes for this level (0 = unlimited) */
static LONG level_minutes(UBYTE level)
{
    char buf[LINELEN], *f[32];
    LONG n, i, best = 60, bestlvl = -1;
    str_copy(buf, cfg_str(N.cfg, "level_minutes", "0:120,255:0"), sizeof(buf));
    n = str_split(buf, ',', f, 32);
    for (i = 0; i < n; i++) {
        char *colon = strchr(f[i], ':');
        LONG lv, mins;
        if (!colon) continue;
        lv = atol(f[i]);
        mins = atol(colon + 1);
        if (lv <= level && lv > bestlvl) { bestlvl = lv; best = mins; }
    }
    return best;
}

LONG time_left_mins(void)
{
    LONG used, left;
    if (!N.loggedin || N.limit_mins <= 0) return -1;
    used = (LONG)((bbs_now() - N.logon) / 60);
    left = N.limit_mins - used;
    return left > 0 ? left : 0;
}

/* pick up what the sysop may have changed meanwhile: level, lock, access, credits */
static void refresh_locked(void)
{
    struct UserRec disk;
    if (userdb_read(N.user.id, &disk)) {
        N.user.level = disk.level;
        N.user.flags = (N.user.flags & ~(UF_LOCKED | UF_NEWUSER)) | (disk.flags & (UF_LOCKED | UF_NEWUSER));
        N.user.aflags = disk.aflags;            /* access settings belong to the sysop */
        memcpy(N.user.group, disk.group, sizeof(N.user.group));
        N.user.credits = disk.credits;
    }
}

void user_refresh(void)
{
    if (!N.loggedin) return;
    ObtainSemaphore(&N.S->userlock);
    refresh_locked();
    ReleaseSemaphore(&N.S->userlock);
}

void user_save(void)
{
    if (!N.loggedin) return;
    ObtainSemaphore(&N.S->userlock);
    refresh_locked();                           /* don't overwrite the sysop's changes */
    userdb_write(&N.user);
    ReleaseSemaphore(&N.S->userlock);
}

static BOOL name_ok(const char *s)
{
    LONG n = strlen(s);
    const char *p;
    if (n < 2 || n >= NAMELEN) return FALSE;
    if (!str_icmp(s, "NEW") || !str_icmp(s, "SYSOP") || !str_icmp(s, "ALL")) return FALSE;
    for (p = s; *p; p++) {
        UBYTE c = *p;
        /* handles reach door command lines and scripts: no Shell characters */
        if (!shell_char_ok(c) || c == '|' || c == ':' || c == '/' || c == '@' || c == '%') return FALSE;
    }
    return TRUE;
}

/* BBS:Config/BannedNames.cfg: handles nobody may sign up as, one per line, case doesn't
 * matter; * matches anything ("*sysop*", "nazi*"); # or ; starts a comment.  Edited in
 * BBSConfig (Banned names), the sysop menu, or any text editor. */
BOOL name_banned(const char *name)
{
    struct LineReader lr;
    char line[80], *t;
    BOOL hit = FALSE;
    if (!lr_open(&lr, "BBS:Config/BannedNames.cfg")) return FALSE;
    while (!hit && lr_gets(&lr, line, sizeof(line)) >= 0) {
        t = str_trim(line);
        if (t[0] && t[0] != '#' && t[0] != ';' && glob_icmp(t, name)) hit = TRUE;
    }
    lr_close(&lr);
    return hit;
}

static BOOL user_online_elsewhere(ULONG id)
{
    int n;
    BOOL busy = FALSE;
    shared_lock(N.S);
    for (n = 0; n < N.S->nodes; n++)
        if (n + 1 != N.node && N.S->node[n].state >= NS_ONLINE &&
            N.S->node[n].userid == (LONG)id) busy = TRUE;
    shared_unlock(N.S);
    return busy;
}

static BOOL login_failed(void)
{
    BOOL banned;
    if (N.local) return FALSE;                  /* nothing to ban at the console */
    shared_lock(N.S);
    banned = ipf_login_failed(N.S, N.ip, bbs_now());
    shared_unlock(N.S);
    if (banned) {
        tputs(L("login.failed.too_many_failed", "\n|12Too many failed logins - this address is now blocked.|07\n"));
        tn_flush();
        bbs_log(BBS_SYSLOG, "node %d: %s auto-banned for failed logins", N.node, N.ipstr);
        node_hangup("failed logins");
    }
    return banned;
}

/* Handles only a password-guessing bot types (trap_names in NilBBS.cfg, default
 * admin/root/administrator).  Anyone trying one is banned for good - unless the
 * address is whitelisted (the sysop's LAN), which only gets it logged.  TRUE =
 * the caller has been banned and hung up. */
static BOOL trap_name(const char *name)
{
    char list[160], *p, *w;
    BOOL hit = FALSE, ok;
    str_copy(list, cfg_str(N.cfg, "trap_names", "admin root administrator"), sizeof(list));
    for (p = list; *p && !hit; ) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        w = p;
        while (*p && *p != ' ' && *p != ',' && *p != '\t') p++;
        if (*p) *p++ = 0;
        if (*w && !str_icmp(w, name)) hit = TRUE;
    }
    if (!hit) return FALSE;
    if (N.local) return FALSE;
    shared_lock(N.S);
    ok = !ipf_whitelisted(N.S, N.ip);
    if (ok) {
        char why[48];
        sprintf(why, "tried login as \"%.20s\"", name);
        ok = ipf_ban(N.S, N.ip, 0, why);
    }
    shared_unlock(N.S);
    bbs_log(BBS_SYSLOG, "node %d: %s tried trap handle \"%s\"%s", N.node, N.ipstr, name,
            ok ? " - banned permanently" : " (whitelisted, not banned)");
    if (!ok) return FALSE;
    tputs(L("login.trap_name.login_incorrect", "\n|12Login incorrect.|07\n"));
    tn_flush();
    node_hangup("trap handle");
    return TRUE;
}

static BOOL new_user(void)
{
    struct UserRec u;
    char pw1[40], pw2[40], buf[80];
    const char *syspw = cfg_str(N.cfg, "newuser_password", "");
    ULONG id;

    if (!cfg_bool(N.cfg, "newuser_allowed", TRUE)) {
        if (!tshowfile("nonewusers"))
            tputs(L("login.new_user.sorry_this_system", "\n|12Sorry, this system is not accepting new users.|07\n"));
        return FALSE;
    }
    if (syspw[0]) {
        tputs(L("login.new_user.new_user_password", "\n|07New user password: "));
        if (tgetline(buf, 40, GL_MASK) < 0) return FALSE;
        if (str_icmp(buf, syspw)) {
            tputs(L("login.new_user.incorrect", "|12Incorrect.|07\n"));
            bbs_log(BBS_SYSLOG, "node %d: bad new-user password from %s", N.node, N.ipstr);
            return FALSE;
        }
    }

    memset(&u, 0, sizeof(u));
    {   /* more than one language on the board: which one, before anything else is asked */
        char pick[LANG_NAMELEN];
        if (lang_pick(pick)) {
            str_copy(u.lang, pick, sizeof(u.lang));
            lang_load(pick, uni_to_cp437);
        }
    }
    tshowfile("newuser");
    for (;;) {
        tputs(L("login.new_user.choose_handle_the", "\n|07Choose a handle |08(the name you'll log in with)|07: |15"));
        if (tgetline(u.name, NAMELEN, GL_NAME) < 0) return FALSE;
        if (!u.name[0]) return FALSE;
        if (trap_name(u.name)) return FALSE;
        if (!name_ok(u.name) || name_banned(u.name)) {
            tputs(L("login.new_user.that_name_cant", "|12That name can't be used.|07\n"));
            if (name_ok(u.name)) bbs_log(BBS_SYSLOG, "node %d: %s tried banned handle \"%s\"", N.node, N.ipstr, u.name);
            continue;
        }
        {
            struct UserRec tmp;
            LONG found;
            ObtainSemaphore(&N.S->userlock);
            found = userdb_find(u.name, &tmp);
            ReleaseSemaphore(&N.S->userlock);
            if (found) { tputs(L("login.new_user.that_handle_is", "|12That handle is taken.|07\n")); continue; }
        }
        break;
    }
    tputs(L("login.new_user.real_name", "|07Real name: |15"));
    if (tgetline(u.realname, LONGNAME, GL_NAME) < 0) return FALSE;
    str_nopipe(u.realname);
    tputs(L("login.new_user.location_city_country", "|07Location |08(city, country)|07: |15"));
    if (tgetline(u.location, LONGNAME, 0) < 0) return FALSE;
    str_nopipe(u.location);
    if (cfg_bool(N.cfg, "ask_email", TRUE)) {
        tputs(L("login.new_user.mail_optional", "|07E-mail |08(optional)|07: |15"));
        if (tgetline(u.email, sizeof(u.email), 0) < 0) return FALSE;
        str_nopipe(u.email);
    }
    for (;;) {
        tputs(L("login.new_user.password_4_characters", "|07Password |08(4+ characters)|07: "));
        if (tgetline(pw1, sizeof(pw1), GL_MASK) < 0) return FALSE;
        if (strlen(pw1) < 4) { tputs(L("login.new_user.too_short", "|12Too short.|07\n")); continue; }
        if (!str_icmp(pw1, u.name)) { tputs(L("login.new_user.dont_use_your", "|12Don't use your handle.|07\n")); continue; }
        tputs(L("login.new_user.again_to_confirm", "|07Again, to confirm: "));
        if (tgetline(pw2, sizeof(pw2), GL_MASK) < 0) return FALSE;
        if (strcmp(pw1, pw2)) { tputs(L("login.new_user.they_dont_match", "|12They don't match.|07\n")); continue; }
        break;
    }

    tprintf(L("login.new_user.your_terminal_was", "\n|07Your terminal was detected as |15%s|07 with |15%s|07 characters.\n"),
            term_name(N.term), charset_name(N.charset));
    if (!tyesno(L("login.new_user.is_that_right", "|07Is that right?"), TRUE)) term_choose();

    u.level     = (UBYTE)cfg_int(N.cfg, "newuser_level", 10);
    u.flags     = UF_HOTKEYS;
    /* validated_level > 0: the account waits at newuser_level until the sysop validates it */
    if (cfg_int(N.cfg, "validated_level", 0) > 0) u.flags |= UF_NEWUSER;
    u.termtype  = N.term;
    u.charset   = N.charset;
    u.cols      = (UBYTE)N.cols;
    u.rows      = (UBYTE)N.rows;
    u.firstcall = bbs_now();
    user_setpass(&u, pw1);

    ObtainSemaphore(&N.S->userlock);
    {
        struct UserRec tmp;
        if (userdb_find(u.name, &tmp)) id = 0;      /* raced another node */
        else {
            /* the very first account on a fresh system is the sysop */
            if (userdb_count() == 0) {
                u.level = (UBYTE)cfg_int(N.cfg, "sysop_level", 255);
                u.flags &= ~UF_NEWUSER;
            }
            id = userdb_add(&u);
        }
    }
    ReleaseSemaphore(&N.S->userlock);
    if (!id) { tputs(L("login.new_user.could_not_create", "|12Could not create the account - please try again.|07\n")); return FALSE; }

    N.user = u;
    bbs_log(BBS_SYSLOG, "node %d: new user %s (#%lu) from %s", N.node, u.name, id, N.ipstr);
    tprintf(L("login.new_user.welcome_aboard_you", "\n|10Welcome aboard, |15%s|10! You are user #%lu.|07\n"), u.name, id);
    if (u.flags & UF_NEWUSER)
        tputs(L("login.new_user.the_sysop_will", "|07The sysop will look over your account soon - until then some areas stay closed.\n"));
    return TRUE;
}

static void default_banner(void)
{
    tcls();
    tprintf("|09%s\n", "");
    tprintf("|11  %s\n", cfg_str(N.cfg, "bbs_name", "NilBBS"));
    tprintf(L("login.default_banner.running_nilbbs_on", "|08  running |15NilBBS %s|08 on AmigaOS  -  node %d\n"), BBS_VERSION, N.node);
    tprintf(L("login.default_banner.your_terminal", "|08  your terminal: |07%s, %s, %dx%d\n\n"), term_name(N.term),
            charset_name(N.charset), (int)N.cols, (int)N.rows);
}

BOOL do_login(void)
{
    LONG tries, max = cfg_int(N.cfg, "max_login_tries", 3);
    char name[NAMELEN], pw[40];
    BOOL ok = FALSE;
    ULONG today;

    if (!tshowfile("connect")) default_banner();

    for (tries = 0; tries < max && N.online && !ok; tries++) {
        tputs(L("login.do_login.handle_or_new", "\n|07Handle |08(or |15NEW|08)|07: |15"));
        if (tgetline(name, NAMELEN, GL_NAME) < 0) return FALSE;
        tputs("|07");
        if (!name[0]) { tries--; continue; }
        if (!str_icmp(name, "NEW")) {
            ok = new_user();
            if (!N.online) return FALSE;
            continue;
        }
        if (trap_name(name)) return FALSE;
        tputs(L("login.do_login.password", "|07Password: "));
        if (tgetline(pw, sizeof(pw), GL_MASK) < 0) return FALSE;
        {
            LONG id;
            ObtainSemaphore(&N.S->userlock);
            id = userdb_find(name, &N.user);
            ReleaseSemaphore(&N.S->userlock);
            if (!id || !user_checkpass(&N.user, pw)) {
                bbs_log(BBS_SYSLOG, "node %d: failed login as \"%s\" from %s", N.node, name, N.ipstr);
                tputs(L("login.do_login.login_incorrect", "|12Login incorrect.|07\n"));
                memset(&N.user, 0, sizeof(N.user));
                if (login_failed()) return FALSE;
                continue;
            }
        }
        if (N.user.flags & UF_LOCKED) {
            tputs(L("login.do_login.this_account_is", "|12This account is locked. Contact the sysop.|07\n"));
            bbs_log(BBS_SYSLOG, "node %d: locked account %s tried to log in", N.node, N.user.name);
            return FALSE;
        }
        if (user_online_elsewhere(N.user.id) &&
            N.user.level < cfg_int(N.cfg, "sysop_level", 255)) {
            tputs(L("login.do_login.youre_already_logged", "|12You're already logged in on another node.|07\n"));
            return FALSE;
        }
        ok = TRUE;
    }
    if (!ok) {
        if (N.online) { tputs(L("login.do_login.goodbye", "\n|12Goodbye.|07\n")); tn_flush(); }
        return FALSE;
    }

    /* ---- logged in ---- */
    shared_lock(N.S);
    ipf_login_ok(N.S, N.ip);
    shared_unlock(N.S);

    today = bbs_daynum(bbs_now());
    if (N.user.today != today) {
        N.user.today = today;
        N.user.mins_today = 0;
        N.user.calls_today = 0;
    }
    N.user.calls++;
    N.user.calls_today++;
    N.prevcall = N.user.lastcall;
    N.user.lastcall = bbs_now();
    N.user.lastip = N.ip;
    if (N.user.flags & UF_TERMSET) {
        N.term = N.user.termtype;
        N.charset = N.user.charset;
    } else {
        N.user.termtype = N.term;
        N.user.charset = N.charset;
    }
    N.sysop = N.user.level >= cfg_int(N.cfg, "sysop_level", 255);
    N.logon = bbs_now();
    N.limit_mins = level_minutes(N.user.level);
    if (N.limit_mins > 0) {
        N.limit_mins -= N.user.mins_today;
        if (N.limit_mins <= 0) {
            tputs(L("login.do_login.youve_used_all", "\n|12You've used all your time for today. Call back tomorrow!|07\n"));
            tn_flush();
            return FALSE;
        }
    }
    N.loggedin = TRUE;
    user_save();

    shared_lock(N.S);
    N.ni->state = NS_ONLINE;
    N.ni->userid = N.user.id;
    str_copy(N.ni->user, N.user.name, NAMELEN);
    str_copy(N.ni->location, N.user.location, LONGNAME);
    N.ni->available = (N.user.flags & UF_NOPAGE) ? 0 : 1;
    N.ni->termtype = N.term;
    shared_unlock(N.S);

    bbs_log(BBS_CALLERLOG, "node %d  %-20s  %-16s  %s/%s", N.node, N.user.name, N.ipstr,
            term_name(N.term), charset_name(N.charset));
    lastcallers_add();
    return TRUE;
}

void do_logoff(void)
{
    if (N.loggedin) {
        ULONG mins = (bbs_now() - N.logon) / 60;
        N.user.mins_today += (UWORD)mins;
        user_save();
        bbs_log(BBS_SYSLOG, "node %d: %s logged off after %lu min", N.node, N.user.name, mins);
    }
    if (N.online) {
        if (!tshowfile("logoff"))
            tprintf(L("login.do_logoff.thanks_for_calling", "\n|11Thanks for calling |15%s|11!|07\n"), cfg_str(N.cfg, "bbs_name", "NilBBS"));
        tcolor(7);
        tn_flush();
    }
}
