/*
 * sysop.c - the remote sysop menu: IP bans, kicking, broadcasts, users, logs.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

static void show_bans(void)
{
    static struct IPBan bans[MAX_BANS];         /* static: too big for the stack */
    static struct IPRule rules[MAX_IPRULES];
    UWORD nrules, i, shown = 0, def;
    ULONG now = bbs_now();

    shared_lock(N.S);
    memcpy(bans, N.S->bans, sizeof(bans));
    memcpy(rules, N.S->rules, sizeof(rules));
    nrules = N.S->nrules;
    def = N.S->default_deny;
    shared_unlock(N.S);

    tpage_start();
    tprintf(L("sysop.show_bans.static_rules_bbs", "\n|15Static rules|07 (BBS:Config/IPFilter.cfg, default |15%s|07)\n"),
            def ? L("sysop.show_bans.deny", "deny") : L("sysop.show_bans.allow", "allow"));
    for (i = 0; i < nrules && tmore(); i++) {
        char r[40];
        ipf_rule_str(&rules[i], r);
        tprintf("  |%s%s|07\n", rules[i].type == IPR_ALLOW ? "10" : "12", r);
    }
    if (!nrules) tputs(L("sysop.show_bans.none", "  |08(none)|07\n"));
    tputs(L("sysop.show_bans.bans", "\n|15Bans|07\n"));
    tputs(L("sysop.show_bans.address_expires_reason", "|08  Address          Expires            Reason\n"
          "  ---------------  -----------------  ------------------------------|07\n"));
    for (i = 0; i < MAX_BANS && tmore(); i++) {
        char ip[16], ex[24];
        if (!bans[i].ip) continue;
        ip_tostr(bans[i].ip, ip);
        if (!bans[i].expires) str_copy(ex, L("sysop.show_bans.never", "never"), sizeof(ex));
        else snprintf(ex, sizeof(ex), L("sysop.show_bans.in_min", "in %lu min"), (bans[i].expires - now + 59) / 60);
        tprintf("  |15%-15s  |07%-17s  |08%s|07\n", ip, ex, bans[i].reason);
        shown++;
    }
    if (!shown) tputs(L("sysop.show_bans.none", "  |08(none)|07\n"));
    tpage_end();
}

static void add_ban(void)
{
    char ipb[20], minb[8], why[40], old[40];
    ULONG ip;
    LONG mins;
    tputs(L("sysop.add_ban.ip_address_to", "\n|07IP address to ban: |15"));
    if (tgetline(ipb, 16, 0) <= 0) return;
    if (!ip_parse(ipb, &ip)) { tputs(L("sysop.add_ban.not_an_ip", "|12Not an IP address.|07\n")); return; }
    old[0] = 0;
    shared_lock(N.S);
    { struct IPBan *b = ipf_findban(N.S, ip); if (b) str_copy(old, b->reason, sizeof(old)); }
    shared_unlock(N.S);
    tputs(L("sysop.add_ban.minutes_0_permanent", "|07Minutes |08(0 = permanent)|07: |15"));
    if (tgetline(minb, 7, GL_DIGITS) < 0) return;
    mins = atol(minb);
    /* already banned: show the reason on file, Enter keeps it */
    if (old[0]) tprintf(L("sysop.add_ban.reason_keep", "|07Reason |08(Enter keeps \"%s\")|07: |15"), old);
    else tputs(L("sysop.add_ban.reason", "|07Reason: |15"));
    if (tgetline(why, 38, 0) < 0) return;
    str_nopipe(why);
    shared_lock(N.S);
    if (ipf_whitelisted(N.S, ip))
        tputs(L("sysop.add_ban.note_that_address", "|14Note: that address is on an allow rule, so the ban won't apply.|07\n"));
    ipf_ban(N.S, ip, (ULONG)mins, why);                 /* "" keeps the old reason / "sysop" */
    { struct IPBan *b = ipf_findban(N.S, ip); if (b) str_copy(why, b->reason, sizeof(why)); }
    shared_unlock(N.S);
    bbs_log(BBS_SYSLOG, "node %d: sysop %s banned %s (%ld min: %s)", N.node, N.user.name, ipb, mins, why);
    tputs(L("sysop.add_ban.banned", "|10Banned.|07\n"));
}

static void remove_ban(void)
{
    char ipb[20];
    ULONG ip;
    BOOL ok;
    tputs(L("sysop.remove_ban.ip_address_to", "\n|07IP address to unban: |15"));
    if (tgetline(ipb, 16, 0) <= 0) return;
    if (!ip_parse(ipb, &ip)) { tputs(L("sysop.remove_ban.not_an_ip", "|12Not an IP address.|07\n")); return; }
    shared_lock(N.S);
    ok = ipf_unban(N.S, ip);
    shared_unlock(N.S);
    if (ok) bbs_log(BBS_SYSLOG, "node %d: sysop %s unbanned %s", N.node, N.user.name, ipb);
    tputs(ok ? L("sysop.remove_ban.unbanned", "|10Unbanned.|07\n") : L("sysop.remove_ban.that_address_isnt", "|12That address isn't banned.|07\n"));
}

static void kick_node(void)
{
    char b[4];
    int n;
    struct Task *t = NULL;
    whos_online();
    tputs(L("sysop.kick_node.disconnect_node", "\n|07Disconnect node: |15"));
    if (tgetline(b, 3, GL_DIGITS) <= 0) return;
    n = atoi(b);
    if (n == N.node) { tputs(L("sysop.kick_node.thats_you", "|12That's you.|07\n")); return; }
    shared_lock(N.S);
    if (n >= 1 && n <= N.S->nodes && N.S->node[n - 1].state >= NS_LOGIN) t = N.S->node[n - 1].task;
    shared_unlock(N.S);
    if (!t) { tputs(L("sysop.kick_node.nobody_there", "|12Nobody there.|07\n")); return; }
    if (tyesno(L("sysop.kick_node.also_ban_their", "|07Also ban their address for an hour?"), FALSE)) {
        char why[40], old[40];
        ULONG ip;
        old[0] = 0;
        shared_lock(N.S);
        ip = N.S->node[n - 1].ip;
        { struct IPBan *b = ipf_findban(N.S, ip); if (b) str_copy(old, b->reason, sizeof(old)); }
        shared_unlock(N.S);
        if (old[0]) tprintf(L("sysop.add_ban.reason_keep", "|07Reason |08(Enter keeps \"%s\")|07: |15"), old);
        else tputs(L("sysop.kick_node.reason", "|07Reason |08(Enter = kicked by sysop)|07: |15"));
        if (tgetline(why, 38, 0) < 0) why[0] = 0;
        str_nopipe(why);
        if (!why[0] && !old[0]) strcpy(why, "kicked by sysop");
        shared_lock(N.S);
        ipf_ban(N.S, ip, 60, why);                      /* "" keeps the old reason */
        shared_unlock(N.S);
    }
    Signal(t, SIGBREAKF_CTRL_C);
    bbs_log(BBS_SYSLOG, "node %d: sysop %s kicked node %d", N.node, N.user.name, n);
    tputs(L("sysop.kick_node.done", "|10Done.|07\n"));
}

/* a node stuck in a door (a crashed door, a script that never ends): free it */
static void reset_node(void)
{
    char b[4];
    int n, i;
    whos_online();
    tputs(L("sysop.reset_node.force_reset_node", "\n|07Force-reset node: |15"));
    if (tgetline(b, 3, GL_DIGITS) <= 0) return;
    n = atoi(b);
    if (n == N.node) { tputs(L("sysop.reset_node.thats_you", "|12That's you.|07\n")); return; }
    if (!tyesno(L("sysop.reset_node.hang_the_caller", "|07Hang the caller up and free the node, even from a door?"), FALSE)) return;
    if (!node_reset_start(N.S, n)) { tputs(L("sysop.reset_node.nobody_there", "|12Nobody there.|07\n")); return; }
    bbs_log(BBS_SYSLOG, "node %d: sysop %s reset node %d", N.node, N.user.name, n);
    tputs(L("sysop.reset_node.waiting_for_the", "|07Waiting for the node"));
    for (i = 0; i < (NODE_RESET_FORCE + 5) * 2 && N.S->node[n - 1].state != NS_FREE; i++) {
        Delay(25);
        if (i % 4 == 3) tputs(".");
    }
    tputs("\n");
    if (N.S->node[n - 1].state != NS_FREE) tputs(L("sysop.reset_node.its_still_busy", "|12It's still busy - try again, or reboot.|07\n"));
    else if (i < NODE_RESET_FORCE * 2) tputs(L("sysop.reset_node.the_node_is", "|10The node is free again.|07\n"));
    else tputs(L("sysop.reset_node.the_node_didnt", "|10The node didn't answer - its slot was freed by force.|07\n"));
}

static void broadcast(void)
{
    char text[110];
    int n;
    tputs(L("sysop.broadcast.broadcast_to_every", "\n|07Broadcast to every node: |15"));
    if (tgetline(text, 100, 0) <= 0) return;
    for (n = 1; n <= N.S->nodes; n++) {
        struct Task *t = NULL;
        struct NodeInfo *ni;
        if (n == N.node) continue;
        shared_lock(N.S);
        ni = &N.S->node[n - 1];
        if (ni->state >= NS_ONLINE) {
            UBYTE next = (ni->msg_head + 1) % NODE_MSGQ;
            if (next != ni->msg_tail) {
                struct NodeMsg *m = &ni->msgq[ni->msg_head];
                m->from = (UBYTE)N.node;
                m->type = NM_SYSOP;
                str_copy(m->fromname, N.user.name, NAMELEN);
                str_copy(m->text, text, sizeof(m->text));
                ni->msg_head = next;
                t = ni->task;
            }
        }
        shared_unlock(N.S);
        if (t) Signal(t, SIGBREAKF_CTRL_D);
    }
    tputs(L("sysop.broadcast.sent", "|10Sent.|07\n"));
}

static void edit_user(void)
{
    struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
    char name[NAMELEN], b[8];
    LONG id;
    if (!u) return;
    tputs(L("sysop.edit_user.user_handle", "\n|07User handle: |15"));
    if (tgetline(name, NAMELEN, 0) <= 0) { FreeVec(u); return; }
    ObtainSemaphore(&N.S->userlock);
    id = userdb_find(name, u);
    ReleaseSemaphore(&N.S->userlock);
    if (!id) { tputs(L("sysop.edit_user.no_such_user", "|12No such user.|07\n")); FreeVec(u); return; }

    for (;;) {
        char ip[16], last[20];
        LONG k;
        ip_tostr(u->lastip, ip);
        bbs_datetimestr(u->lastcall, last);
        tprintf("\n|15#%lu %s|07  (%s, %s)\n", u->id, u->name, u->realname, u->location);
        tprintf(L("sysop.edit_user.level_calls_posts", "  |07Level |15%d|07   Calls |15%lu|07   Posts |15%lu|07   Last |15%s|07 from |15%s|07\n"),
                (int)u->level, u->calls, u->posts, last, ip);
        {
            char fl[28];
            int i, n = 0;
            for (i = 0; i < 26; i++) if (u->aflags & (1UL << i)) fl[n++] = (char)('A' + i);
            fl[n] = 0;
            tprintf(L("sysop.edit_user.flags_group_credits", "  |07Flags |15%s|07   Group |15%s|07   Credits |15%ld|07 KB   UL/DL |15%lu|07/|15%lu|07 KB\n"),
                    n ? fl : "-", u->group[0] ? u->group : "-", u->credits, u->ulkb, u->dlkb);
        }
        tprintf(L("sysop.edit_user.status", "  |07Status: |15%s%s%s|07\n"), (u->flags & UF_LOCKED) ? L("sysop.edit_user.locked", "LOCKED ") : L("sysop.edit_user.active", "active "),
                (u->flags & UF_HIDDEN) ? L("sysop.edit_user.hidden_not_in", "HIDDEN (not in last callers) ") : "",
                (u->flags & UF_DELETED) ? L("sysop.edit_user.deleted", "DELETED") : "");
        tputs(L("sysop.edit_user.level_flags_group", "  |08[|15L|08]evel [|15F|08]lags [|15G|08]roup [|15C|08]redits [|15K|08]lock/unlock [|15H|08]idden\n"
              "  |08[|15D|08]elete [|15P|08]assword [|15Q|08]uit: |07"));
        k = tgethot("LFGCKHDPQ\r");
        if (k == KEY_HANGUP) break;
        tprintf("%c\n", (int)(k == '\r' ? 'Q' : k));
        if (k == 'L') {
            tputs(L("sysop.edit_user.new_level_0", "|07New level (0-255): |15"));
            if (tgetline(b, 4, GL_DIGITS) > 0 && atoi(b) <= 255) u->level = (UBYTE)atoi(b);
        } else if (k == 'F') {
            char fl[30], *p;
            tputs(L("sysop.edit_user.flags_letters_to", "|07Flags |08(letters A-Z to toggle, e.g. AD)|07: |15"));
            if (tgetline(fl, 27, GL_UPPER) > 0)
                for (p = fl; *p; p++) if (*p >= 'A' && *p <= 'Z') u->aflags ^= 1UL << (*p - 'A');
        } else if (k == 'G') {
            tputs(L("sysop.edit_user.group_enter_none", "|07Group |08(Enter = none)|07: |15"));
            tgetline(u->group, sizeof(u->group), GL_EDIT);
        } else if (k == 'C') {
            char cb[10];
            tputs(L("sysop.edit_user.download_credits_in", "|07Download credits in KB: |15"));
            if (tgetline(cb, 8, GL_DIGITS) > 0) u->credits = atol(cb);
        } else if (k == 'K') u->flags ^= UF_LOCKED;
        else if (k == 'H') {
            u->flags ^= UF_HIDDEN;
            if (u->flags & UF_HIDDEN) {             /* and off the list it's already on */
                ObtainSemaphore(&N.S->msglock); lastcallers_remove(u->name); ReleaseSemaphore(&N.S->msglock);
            }
        } else if (k == 'D') {
            if (tyesno(L("sysop.edit_user.really_delete_this", "|12Really delete this user?|07"), FALSE)) u->flags |= UF_DELETED;
        } else if (k == 'P') {
            char pw[40];
            tputs(L("sysop.edit_user.new_password", "|07New password: "));
            if (tgetline(pw, 40, GL_MASK) >= 4) user_setpass(u, pw);
        } else break;
        ObtainSemaphore(&N.S->userlock);
        userdb_write(u);
        ReleaseSemaphore(&N.S->userlock);
        bbs_log(BBS_SYSLOG, "node %d: sysop %s edited user %s", N.node, N.user.name, u->name);
    }
    FreeVec(u);
}

/* ---- new-user validation (validated_level) ----------------------------------
 * With validated_level set, a new sign-up gets newuser_level and UF_NEWUSER;
 * here the sysop walks the waiting accounts and validates (level ->
 * validated_level), deletes, locks or skips each one. */
int users_waiting(void)
{
    struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
    LONG i, n;
    int w = 0;
    if (!u) return 0;
    ObtainSemaphore(&N.S->userlock);
    n = userdb_count();
    for (i = 1; i <= n; i++)
        if (userdb_read(i, u) && (u->flags & UF_NEWUSER) && !(u->flags & UF_DELETED)) w++;
    ReleaseSemaphore(&N.S->userlock);
    FreeVec(u);
    return w;
}

static BOOL user_online(const char *name)
{
    int i;
    for (i = 0; i < N.S->nodes && i < MAX_NODES; i++)
        if (N.S->node[i].state != NS_FREE && !str_icmp(N.S->node[i].user, name)) return TRUE;
    return FALSE;
}

static void validate_users(void)
{
    struct UserRec *u = AllocVec(sizeof(struct UserRec), 0);
    LONG vl = cfg_int(N.cfg, "validated_level", 0), i, n, seen = 0;
    if (!u) return;
    if (vl <= 0) {
        tputs(L("sysop.validate_users.validated_level_isnt", "|08validated_level isn't set (NilBBS.cfg / BBSConfig New users), so new users\n"
              "start at newuser_level and nothing waits here.|07\n"));
    }
    ObtainSemaphore(&N.S->userlock);
    n = userdb_count();
    ReleaseSemaphore(&N.S->userlock);
    for (i = 1; i <= n && N.online; i++) {
        char ip[16], first[20];
        LONG k;
        BOOL ok;
        ObtainSemaphore(&N.S->userlock);
        ok = userdb_read(i, u);
        ReleaseSemaphore(&N.S->userlock);
        if (!ok || !(u->flags & UF_NEWUSER) || (u->flags & UF_DELETED)) continue;
        seen++;
        ip_tostr(u->lastip, ip);
        bbs_datetimestr(u->firstcall, first);
        tprintf(L("sysop.validate_users.level", "\n|15#%lu %s|07  level |15%d|07%s\n"), u->id, u->name, (int)u->level,
                user_online(u->name) ? L("sysop.validate_users.online_now", "  |14(online now)|07") : "");
        tprintf(L("sysop.validate_users.real_name_from", "  |07Real name |15%s|07   From |15%s|07\n"), u->realname[0] ? u->realname : "-",
                u->location[0] ? u->location : "-");
        tprintf(L("sysop.validate_users.mail", "  |07E-mail |15%s|07\n"), u->email[0] ? u->email : "-");
        tprintf(L("sysop.validate_users.signed_up_from", "  |07Signed up |15%s|07 from |15%s|07   Calls |15%lu|07\n"), first, ip, u->calls);
        tprintf(L("sysop.validate_users.validate_level_delete", "  |08[|15V|08]alidate (level %ld)  [|15D|08]elete  [|15L|08]ock  [|15S|08]kip  [|15Q|08]uit: |07"),
                vl > 0 ? vl : (LONG)u->level);
        k = tgethot("VDLSQ\r");
        if (k == KEY_HANGUP) break;
        tprintf("%c\n", (int)(k == '\r' ? 'S' : k));
        if (k == 'Q') break;
        if (k == 'S' || k == '\r') continue;
        if (k == 'D' && !tyesno(L("sysop.validate_users.really_delete_this", "|12Really delete this user?|07"), FALSE)) continue;
        ObtainSemaphore(&N.S->userlock);
        if (userdb_read(i, u)) {                    /* re-read: they may be online */
            if (k == 'V') {
                if (vl > 0) u->level = (UBYTE)vl;
                u->flags &= ~UF_NEWUSER;
            } else if (k == 'D') u->flags = (u->flags | UF_DELETED) & ~UF_NEWUSER;
            else if (k == 'L') u->flags = (u->flags | UF_LOCKED) & ~UF_NEWUSER;
            userdb_write(u);
        }
        ReleaseSemaphore(&N.S->userlock);
        bbs_log(BBS_SYSLOG, "node %d: sysop %s %s new user %s", N.node, N.user.name,
                k == 'V' ? "validated" : k == 'D' ? "deleted" : "locked", u->name);
        tprintf("|10%s.|07%s\n", k == 'V' ? L("sysop.validate_users.validated", "Validated") : k == 'D' ? L("sysop.validate_users.deleted", "Deleted") : L("sysop.validate_users.locked", "Locked"),
                user_online(u->name) ? L("sysop.validate_users.theyre_online_it", "  |08(they're online: it takes hold at their next menu)|07") : "");
    }
    if (!seen) tputs(L("sysop.validate_users.nobody_is_waiting", "|08Nobody is waiting.|07\n"));
    FreeVec(u);
}

/* last ~20 lines of a log file */
static void tail_log(const char *path)
{
    LONG size = file_size(path), start, n;
    UBYTE *buf, *p;
    BPTR fh;
    int lines = 0;
    if (size <= 0) { tputs(L("sysop.tail_log.empty", "|08(empty)|07\n")); return; }
    start = size > 3000 ? size - 3000 : 0;
    if (!(buf = AllocVec(3001, 0))) return;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) { FreeVec(buf); return; }
    Seek(fh, start, OFFSET_BEGINNING);
    n = Read(fh, buf, 3000);
    Close(fh);
    if (n < 0) n = 0;
    buf[n] = 0;
    for (p = buf + n; p > buf; p--) if (*p == '\n' && ++lines > (int)N.rows - 4) { p++; break; }
    tputs("\n|07");
    tputraw(p, (buf + n) - p, CS_LATIN1);
    FreeVec(buf);
}

/* BBS:Logs/Bans.log newest first, paged; skips the # notes and the --- marker */
#define BANLOG_TAIL  16384
#define BANLOG_LINES 400
static void show_ban_log(void)
{
    LONG size = file_size(BBS_BANLOG), start, n, cnt = 0, i;
    char *buf, *p, **ln;
    BPTR fh;
    if (size <= 0) { tputs(L("sysop.ban_log.empty", "\n  |08(the ban log is empty)|07\n")); return; }
    start = size > BANLOG_TAIL ? size - BANLOG_TAIL : 0;
    if (!(buf = AllocVec(BANLOG_TAIL + 4 + BANLOG_LINES * sizeof(char *), 0))) return;
    ln = (char **)(((ULONG)(buf + BANLOG_TAIL + 1) + 3) & ~3UL);   /* pointer table after the text */
    if (!(fh = Open((STRPTR)BBS_BANLOG, MODE_OLDFILE))) { FreeVec(buf); return; }
    Seek(fh, start, OFFSET_BEGINNING);
    n = Read(fh, buf, BANLOG_TAIL);
    Close(fh);
    if (n < 0) n = 0;
    buf[n] = 0;
    p = buf;
    if (start) { while (*p && *p != '\n') p++; if (*p) p++; }   /* drop the partial first line */
    while (*p) {
        char *e = p, *t;
        while (*e && *e != '\n') e++;
        if (*e) *e++ = 0;
        if ((t = strstr(p, "   [from System.log]"))) *t = 0;
        if (*p && *p != '#' && strncmp(p, "---", 3)) {
            if (cnt == BANLOG_LINES) { memmove(ln, ln + 1, (BANLOG_LINES - 1) * sizeof(char *)); cnt--; }
            ln[cnt++] = p;
        }
        p = e;
    }
    tputs(L("sysop.ban_log.title", "\n|15Ban log|07 (BBS:Logs/Bans.log, newest first)\n"));
    tpage_start();
    for (i = cnt - 1; i >= 0 && tmore(); i--) {
        const char *c = strstr(ln[i], "UNBAN") ? "10" : strstr(ln[i], "EXPIRED") ? "08" : "12";
        tprintf("|%s", c);
        tputraw((UBYTE *)ln[i], strlen(ln[i]), CS_LATIN1);
        tputs("|07\n");
    }
    if (!cnt) tputs(L("sysop.ban_log.empty", "\n  |08(the ban log is empty)|07\n"));
    tpage_end();
    FreeVec(buf);
}

/* run BBSMaint IMPORT and show the sysop what it did */
static void import_now(void)
{
    char out[64];
    BPTR in, fh;
    tputs(L("sysop.import_now.importing_this_can", "\n|07Importing - this can take a moment for big archives...\n"));
    tn_flush();
    sprintf(out, "BBS:Nodes/Node%d/import.txt", N.node);
    in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    if (!(fh = Open((STRPTR)out, MODE_NEWFILE))) { Close(in); return; }
    SystemTags((STRPTR)"BBS:BBSMaint IMPORT", SYS_Input, in, SYS_Output, fh, NP_StackSize, 32768, TAG_END);
    Close(in);
    Close(fh);
    tpage_start();
    tshowpath(out, CS_LATIN1);
    tpage_end();
    DeleteFile((STRPTR)out);
    file_areas_load();
}

/* ---- one-liners + last callers ------------------------------------------------ */

static void ol_view(void)
{
    OneLine *a;
    LONG n, i;
    ObtainSemaphore(&N.S->msglock);
    n = oneliners_load(&a);
    ReleaseSemaphore(&N.S->msglock);
    if (!n) { tputs(L("sysop.ol_view.no_one_liners", "\n  |08(no one-liners)|07\n")); return; }
    tputs("\n");
    tpage_start();
    for (i = 0; i < n && tmore(); i++) {
        char *f[3], line[OL_LINE];
        str_copy(line, a[i], sizeof(line));
        if (str_split(line, '|', f, 3) < 3) tprintf("  |15%4ld  |07%s\n", i + 1, a[i]);
        else tprintf("  |15%4ld  |11%-16.16s |08%-9.9s |07%.44s\n", i + 1, f[0], f[1], f[2]);
    }
    tpage_end();
    FreeVec(a);
}

/* "3 5-7,9" -> del[3], del[5..7], del[9] set; returns how many numbers */
static LONG parse_numbers(const char *s, UBYTE *del, LONG max)
{
    LONG cnt = 0;
    while (*s) {
        LONG a, b;
        while (*s && (*s < '0' || *s > '9')) s++;
        if (!*s) break;
        a = atol(s);
        while (*s >= '0' && *s <= '9') s++;
        b = a;
        if (*s == '-') { s++; if (*s >= '0' && *s <= '9') { b = atol(s); while (*s >= '0' && *s <= '9') s++; } }
        if (b < a) { LONG t = a; a = b; b = t; }
        for (; a <= b; a++) if (a >= 1 && a < max && !del[a]) { del[a] = 1; cnt++; }
    }
    return cnt;
}

static void ol_delete(void)
{
    char buf[80];
    UBYTE *del;
    LONG n, cnt, gone;
    OneLine *a;
    LONG i, first, one = 0;
    ObtainSemaphore(&N.S->msglock);
    n = oneliners_load(&a);
    ReleaseSemaphore(&N.S->msglock);
    if (!n) { tputs(L("sysop.ol_delete.no_one_liners", "|08No one-liners.|07\n")); return; }
    /* the newest ones (what callers see) with their numbers, so one bad line is easy to pick */
    first = n - ((LONG)N.rows - 8 > 5 ? (LONG)N.rows - 8 : 5);
    if (first < 0) first = 0;
    tputs("\n");
    if (first > 0) tprintf(L("sysop.ol_delete.older_lists_them", "  |08(%ld older - V lists them all)|07\n"), first);
    for (i = first; i < n; i++) {
        char *f[3], line[OL_LINE];
        str_copy(line, a[i], sizeof(line));
        if (str_split(line, '|', f, 3) < 3) tprintf("  |15%4ld  |07%.70s\n", i + 1, a[i]);
        else tprintf("  |15%4ld  |11%-16.16s |07%.52s\n", i + 1, f[0], f[2]);
    }
    tprintf(L("sysop.ol_delete.number_to_delete", "\n|07Number(s) to delete, 1-%ld |08(e.g. 12, or 3 5-7)|07: |15"), n);
    if (tgetline(buf, 70, 0) <= 0) { FreeVec(a); return; }
    if (!(del = AllocVec(n + 2, MEMF_CLEAR))) { FreeVec(a); return; }
    cnt = parse_numbers(buf, del, n + 1);
    if (cnt == 1) {                                 /* one: show which line it is */
        for (i = 1; i <= n; i++) if (del[i]) one = i;
        tprintf("|07#%ld: |15%.70s\n", one, a[one - 1]);
    }
    FreeVec(a);
    if (cnt && tyesno(cnt == 1 ? L("sysop.ol_delete.delete_this_one", "|12Delete this one-liner?|07") : L("sysop.ol_delete.delete_them", "|12Delete them?|07"), FALSE)) {
        ObtainSemaphore(&N.S->msglock);
        gone = oneliners_delete(del, n + 1);
        ReleaseSemaphore(&N.S->msglock);
        tprintf(gone == 1 ? L("sysop.ol_delete.deleted_one", "|10Deleted %ld one-liner.|07\n") : L("sysop.ol_delete.deleted_many", "|10Deleted %ld one-liners.|07\n"), gone);
        bbs_log(BBS_SYSLOG, "node %d: sysop %s deleted %ld one-liner(s)", N.node, N.user.name, gone);
    }
    FreeVec(del);
}

static void lists_menu(void)
{
    set_activity("Sysop: one-liners");
    for (;;) {
        OneLine *a;
        struct LastCall lc[MAX_LASTCALL];
        LONG nol, nlc, k, keep = cfg_int(N.cfg, "oneliners_keep", 50);
        char buf[NAMELEN];
        ObtainSemaphore(&N.S->msglock);
        nol = oneliners_load(&a);
        nlc = lastcallers_load(lc);
        ReleaseSemaphore(&N.S->msglock);
        if (a) FreeVec(a);
        tprintf(L("sysop.lists_menu.one_liners_and", "\n|09-=[ |15One-liners and Last Callers|09 ]=-|07\n\n"
                "  |07One-liners: |15%ld|07 |08(callers see the newest 15; the nightly trim keeps %ld)|07\n"
                "  |07Last callers: |15%ld|07 of %d\n\n"
                "  |08[|15V|08] |07View one-liners             |08[|15D|08] |07Delete one-liners by number\n"
                "  |08[|15T|08] |07Trim to the newest ...      |08[|15X|08] |07Clear all one-liners\n"
                "  |08[|15C|08] |07View last callers           |08[|15R|08] |07Remove a handle from last callers\n"
                "  |08[|15L|08] |07Clear last callers          |08[|15Q|08] |07Back\n\n|07Choice: "),
                nol, keep, nlc, MAX_LASTCALL);
        k = tgethot("VDTXCRLQ\r");
        if (k == KEY_HANGUP) return;
        tprintf("%c\n", (int)(k == '\r' ? 'Q' : k));
        if (k == 'V') ol_view();
        else if (k == 'D') ol_delete();
        else if (k == 'T') {
            char nb[8];
            LONG gone;
            tprintf(L("sysop.lists_menu.keep_the_newest", "|07Keep the newest how many? |08(Enter = %ld)|07: |15"), keep);
            if (tgetline(nb, 6, GL_DIGITS) > 0) keep = atol(nb);
            if (keep <= 0) { tputs(L("sysop.lists_menu.nothing_trimmed", "|08Nothing trimmed.|07\n")); continue; }
            ObtainSemaphore(&N.S->msglock);
            gone = oneliners_trim(keep);
            ReleaseSemaphore(&N.S->msglock);
            tprintf(gone == 1 ? L("sysop.lists_menu.trimmed_one", "|10Trimmed %ld old one-liner.|07\n") : L("sysop.lists_menu.trimmed_many", "|10Trimmed %ld old one-liners.|07\n"), gone);
            if (gone) bbs_log(BBS_SYSLOG, "node %d: sysop %s trimmed one-liners to %ld", N.node, N.user.name, keep);
        } else if (k == 'X') {
            if (tyesno(L("sysop.lists_menu.delete_all_one", "|12Delete ALL one-liners?|07"), FALSE)) {
                ObtainSemaphore(&N.S->msglock); oneliners_clear(); ReleaseSemaphore(&N.S->msglock);
                tputs(L("sysop.lists_menu.one_liners_cleared", "|10One-liners cleared.|07\n"));
                bbs_log(BBS_SYSLOG, "node %d: sysop %s cleared the one-liners", N.node, N.user.name);
            }
        } else if (k == 'C') lastcallers_show();
        else if (k == 'R') {
            LONG gone;
            tputs(L("sysop.lists_menu.handle_to_remove", "|07Handle to remove: |15"));
            if (tgetline(buf, NAMELEN, 0) <= 0) continue;
            ObtainSemaphore(&N.S->msglock);
            gone = lastcallers_remove(buf);
            ReleaseSemaphore(&N.S->msglock);
            tprintf(gone == 1 ? L("sysop.lists_menu.removed_one", "|10Removed %ld entry.|07\n") : L("sysop.lists_menu.removed_many", "|10Removed %ld entries.|07\n"), gone);
            if (gone) bbs_log(BBS_SYSLOG, "node %d: sysop %s removed %s from last callers", N.node, N.user.name, buf);
        } else if (k == 'L') {
            if (tyesno(L("sysop.lists_menu.clear_the_last", "|12Clear the last-callers list?|07"), FALSE)) {
                ObtainSemaphore(&N.S->msglock); lastcallers_clear(); ReleaseSemaphore(&N.S->msglock);
                tputs(L("sysop.lists_menu.last_callers_cleared", "|10Last callers cleared.|07\n"));
                bbs_log(BBS_SYSLOG, "node %d: sysop %s cleared last callers", N.node, N.user.name);
            }
        } else break;
    }
    set_activity("Sysop menu");
}

/* ---- banned handles: BBS:Config/BannedNames.cfg (one pattern a line, * = anything) ---- */
#define BN_FILE "BBS:Config/BannedNames.cfg"
#define BN_MAX  200
BOOL name_banned(const char *name);             /* login.c */

static LONG bn_load(char (*line)[64], LONG max)  /* every line, comments too */
{
    struct LineReader lr;
    LONG n = 0;
    if (!lr_open(&lr, BN_FILE)) return 0;
    while (n < max && lr_gets(&lr, line[n], 64) >= 0) n++;
    lr_close(&lr);
    return n;
}
static BOOL bn_save(char (*line)[64], LONG n)
{
    FILE *f;
    LONG i;
    if (!(f = fopen(BN_FILE ".new", "w"))) return FALSE;
    for (i = 0; i < n; i++) fprintf(f, "%s\n", line[i]);
    fclose(f);
    DeleteFile((STRPTR)BN_FILE);
    return Rename((STRPTR)BN_FILE ".new", (STRPTR)BN_FILE) != 0;
}
static BOOL bn_entry(const char *s) { return s[0] && s[0] != '#' && s[0] != ';'; }

static void banned_menu(void)
{
    char (*line)[64] = AllocVec(BN_MAX * 64, MEMF_CLEAR);
    if (!line) return;
    set_activity("Sysop: banned handles");
    for (;;) {
        LONG n = bn_load(line, BN_MAX), i, e, k;
        char buf[64];
        tputs(L("sysop.banned_menu.banned_handles_nobody", "\n|09-=[ |15Banned handles|09 ]=-|07  |08(nobody can sign up as these; * = anything)|07\n\n"));
        for (i = e = 0; i < n; i++)
            if (bn_entry(str_trim(line[i]))) {
                e++;
                tprintf("  |08%3ld|07 %-24.24s%s", e, str_trim(line[i]), e % 3 ? "" : "\n");
            }
        if (!e) tputs(L("sysop.banned_menu.none_yet", "  |08(none yet)|07"));
        tputs(L("sysop.banned_menu.add_delete_test", "\n\n  |08[|15A|08] |07Add   |08[|15D|08] |07Delete   |08[|15T|08] |07Test a handle   |08[|15Q|08] |07Back\n\n|07Choice: "));
        k = tgethot("ADTQ\r");
        if (k == KEY_HANGUP) break;
        tprintf("%c\n", (int)(k == '\r' ? 'Q' : k));
        if (k == 'A') {
            tputs(L("sysop.banned_menu.handle_or_pattern", "|07Handle or pattern |08(e.g. *sysop*)|07: |15"));
            if (tgetline(buf, NAMELEN + 4, 0) <= 0 || !bn_entry(str_trim(buf))) continue;
            if (n >= BN_MAX) { tputs(L("sysop.banned_menu.the_list_is", "|12The list is full.|07\n")); continue; }
            str_copy(line[n++], str_trim(buf), 64);
            if (bn_save(line, n)) {
                tprintf(L("sysop.banned_menu.banned", "|10Banned: %s|07\n"), str_trim(buf));
                bbs_log(BBS_SYSLOG, "node %d: sysop %s banned the handle %s", N.node, N.user.name, str_trim(buf));
            } else tprintf(L("sysop.banned_menu.write_failed", "|12Could not write %s.|07\n"), BN_FILE);
        } else if (k == 'D') {
            LONG want, j;
            tputs(L("sysop.banned_menu.delete_which_number", "|07Delete which number? |15"));
            if (tgetline(buf, 4, GL_DIGITS) <= 0 || (want = atol(buf)) < 1) continue;
            for (i = j = 0; i < n; i++)
                if (bn_entry(str_trim(line[i])) && ++j == want) break;
            if (i >= n) { tputs(L("sysop.banned_menu.no_such_number", "|12No such number.|07\n")); continue; }
            str_copy(buf, str_trim(line[i]), sizeof(buf));
            for (; i + 1 < n; i++) memcpy(line[i], line[i + 1], 64);
            if (bn_save(line, n - 1)) {
                tprintf(L("sysop.banned_menu.unbanned", "|10Unbanned: %s|07\n"), buf);
                bbs_log(BBS_SYSLOG, "node %d: sysop %s unbanned the handle %s", N.node, N.user.name, buf);
            }
        } else if (k == 'T') {
            tputs(L("sysop.banned_menu.handle_to_test", "|07Handle to test: |15"));
            if (tgetline(buf, NAMELEN, 0) <= 0) continue;
            tprintf(name_banned(buf) ? L("sysop.banned_menu.is_banned", "|12%s is banned.|07\n") : L("sysop.banned_menu.is_allowed", "|10%s is allowed.|07\n"), buf);
        } else break;
    }
    FreeVec(line);
    set_activity("Sysop menu");
}

void sysop_menu(void)
{
    set_activity("Sysop menu");
    for (;;) {
        LONG k;
        tputs(L("sysop.menu.sysop_menu_show", "\n|09-=[ |15Sysop Menu|09 ]=-|07\n\n"
              "  |08[|15B|08] |07Show IP rules and bans      |08[|15A|08] |07Ban an address\n"
              "  |08[|15U|08] |07Unban an address            |08[|15R|08] |07Reload IPFilter.cfg\n"
              "  |08[|15W|08] |07Who's online                |08[|15K|08] |07Disconnect a node\n"
              "  |08[|15M|08] |07Broadcast a message         |08[|15E|08] |07Edit a user\n"
              "  |08[|15L|08] |07System log                  |08[|15C|08] |07Callers log\n"
              "  |08[|15Z|08] |07Create/replace a FILE_ID.DIZ   |08[|15H|08] |07Chat with a node (break in)\n"
              "  |08[|15I|08] |07Import new files (BBS:Files/Import) |08[|15N|08] |07Review new uploads\n"
              "  |08[|15O|08] |07One-liners and last callers |08[|15X|08] |07Banned handles\n"
              "  |08[|15F|08] |07Force-reset a stuck node    |08[|15V|08] |07Validate new users\n"
              "  |08[|15G|08] |07Ban log (bans/unbans)       |08[|15Q|08] |07Back\n\n|07Sysop: "));
        k = tgethot("BAURWKMELCZHINOXFVGQ\r");
        if (k == KEY_HANGUP) return;
        tprintf("%c\n", (int)(k == '\r' ? 'Q' : k));
        switch (k) {
        case 'B': show_bans(); break;
        case 'A': add_ban(); break;
        case 'U': remove_ban(); break;
        case 'G': show_ban_log(); break;
        case 'R':
            if (N.S->daemon) Signal(N.S->daemon, SIGBREAKF_CTRL_F);
            tputs(L("sysop.menu.asked_the_daemon", "|10Asked the daemon to reload the IP filter.|07\n"));
            break;
        case 'W': whos_online(); break;
        case 'K': kick_node(); break;
        case 'F': reset_node(); break;
        case 'M': broadcast(); break;
        case 'E': edit_user(); break;
        case 'L': tail_log(BBS_SYSLOG); break;
        case 'C': tail_log(BBS_CALLERLOG); break;
        case 'H': sysop_chat(); break;
        case 'I': import_now(); break;
        case 'Z': file_areas_load(); file_sysop_diz(); break;
        case 'N': file_areas_load(); file_review_uploads(); break;
        case 'O': lists_menu(); break;
        case 'X': banned_menu(); break;
        case 'V': validate_users(); break;
        default:
            back_to_menu();
            return;
        }
    }
}
