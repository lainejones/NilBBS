/*
 * tele.c - teleconference (multi-user chat), sysop chat and paging the sysop.
 *
 * Every node shares one ring of TELE_RING lines in the shared block.  A node
 * in the teleconference remembers the last sequence number it has shown;
 * posting a line signals everyone in the channel (CTRL-D), whose tgetkey()
 * wakes up with KEY_NONE, and they print whatever is new above their prompt.
 *
 * Channels 1..99 are public.  Private chats (the sysop breaking in on a
 * caller) use channel 100 + the caller's node, and end when either side leaves.
 * A caller inviting another to a private chat (caller_chat) uses 150 + the
 * inviter's node: "#invite:<chan>" goes to the other node, which answers
 * "#accept:<chan>" or "#decline:<chan>" - the inviter waits up to 45 s.
 */
#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "node.h"

#define PRIVATE_BASE 100
#define CALLER_BASE  150        /* caller-to-caller private chats: 150 + the inviting node */

static void tele_loop(UBYTE chan);
static ULONG seen;              /* last sequence number shown */
static UBYTE mychan;
static BOOL  in_tele;
static int   invite_chan, invite_answer;    /* our open invitation; 1 = accepted, 2 = declined */

/* add a line to the ring and wake the nodes that should see it */
static void tele_post(UBYTE chan, UBYTE kind, UBYTE tonode, const char *text)
{
    struct Task *wake[MAX_NODES], *ctl;
    int n, nw = 0;
    struct TeleLine *l;
    shared_lock(N.S);
    l = &N.S->tele[N.S->tele_seq % TELE_RING];
    l->seq = N.S->tele_seq++;
    l->chan = chan;
    l->fromnode = (UBYTE)N.node;
    l->kind = kind;
    l->tonode = tonode;
    str_copy(l->from, N.user.name, NAMELEN);
    str_copy(l->text, text, sizeof(l->text));
    for (n = 0; n < N.S->nodes; n++) {
        struct NodeInfo *ni = &N.S->node[n];
        if (n + 1 == N.node || ni->state < NS_ONLINE || ni->tele != chan || !ni->task) continue;
        if (kind == TK_WHISPER && tonode != n + 1) continue;
        wake[nw++] = ni->task;
    }
    ctl = N.S->ctl_task && N.S->ctl_chat == chan ? N.S->ctl_task : NULL;   /* the sysop at the Amiga */
    shared_unlock(N.S);
    for (n = 0; n < nw; n++) Signal(wake[n], SIGBREAKF_CTRL_D);
    if (ctl) Signal(ctl, SIGBREAKF_CTRL_F);
}

static void show_line(struct TeleLine *l)
{
    BOOL mine = l->fromnode == N.node;
    switch (l->kind) {
    case TK_SAY:
        tprintf("|%s%s|08: |07", mine ? "03" : "11", l->from);
        break;
    case TK_ACTION:
        tprintf("|13* %s ", l->from);
        break;
    case TK_WHISPER:
        tprintf(L("tele.show_line.whispers", "|05(%s whispers)|08: |07"), l->from);
        break;
    default:
        tputs("|08*** ");
    }
    tputraw((const UBYTE *)l->text, strlen(l->text), CS_CP437);
    tputs("|07\n");
}

/* print new lines for our channel; returns TRUE if the other side of a
 * private chat has left */
static BOOL show_new(const char *partial, LONG plen)
{
    static struct TeleLine batch[TELE_RING];
    int nb = 0, i;
    BOOL ended = FALSE;
    ULONG s;
    shared_lock(N.S);
    if (N.S->tele_seq - seen > TELE_RING) seen = N.S->tele_seq - TELE_RING;
    for (s = seen; s < N.S->tele_seq; s++) {
        struct TeleLine *l = &N.S->tele[s % TELE_RING];
        if (l->chan != mychan || l->fromnode == N.node) continue;
        if (l->kind == TK_WHISPER && l->tonode != N.node) continue;
        batch[nb++] = *l;
    }
    seen = N.S->tele_seq;
    shared_unlock(N.S);
    if (!nb) return FALSE;
    tputs("\r");
    tcleol();
    for (i = 0; i < nb; i++) {
        show_line(&batch[i]);
        if (batch[i].kind == TK_LEAVE && mychan >= PRIVATE_BASE) ended = TRUE;
    }
    if (!ended) {
        tputs("|15> |07");
        tputraw((const UBYTE *)partial, plen, CS_CP437);
    }
    return ended;
}

static void who_here(void)
{
    int n;
    tputs(L("tele.who_here.in_this_channel", "|08In this channel:|07"));
    shared_lock(N.S);
    for (n = 0; n < N.S->nodes; n++) {
        struct NodeInfo *ni = &N.S->node[n];
        if (ni->state >= NS_ONLINE && ni->tele == mychan)
            tprintf(" |11%s|08(%d)|07", ni->user, n + 1);
    }
    shared_unlock(N.S);
    tputs("\n");
}

static int node_by_name(const char *name)
{
    int n, found = 0;
    shared_lock(N.S);
    for (n = 0; n < N.S->nodes; n++)
        if (N.S->node[n].state >= NS_ONLINE && N.S->node[n].tele == mychan &&
            !str_icmp(N.S->node[n].user, name)) found = n + 1;
    shared_unlock(N.S);
    return found;
}

static void set_chan(UBYTE chan)
{
    char t[80];
    mychan = chan;
    shared_lock(N.S);
    N.ni->tele = chan;
    seen = N.S->tele_seq;
    shared_unlock(N.S);
    sprintf(t, "%s has joined", N.user.name);
    tele_post(chan, TK_SYSTEM, 0, t);
}

static void leave_chan(void)
{
    char t[80];
    sprintf(t, "%s has left", N.user.name);
    tele_post(mychan, TK_LEAVE, 0, t);
    shared_lock(N.S);
    N.ni->tele = 0;
    shared_unlock(N.S);
}

/* the chat itself; chan >= PRIVATE_BASE is a private two-person chat */
static void tele_loop(UBYTE chan)
{
    char line[152];
    LONG len = 0;
    BOOL private = chan >= PRIVATE_BASE;

    in_tele = TRUE;
    set_activity(chan >= CALLER_BASE ? "Private chat" : private ? "Chatting with the sysop" : "Teleconference");
    set_chan(chan);
    if (private) tputs(L("tele.loop.chat_mode_type", "\n|14Chat mode.|07 Type to talk; |15/q|07 ends the chat.\n"));
    else {
        tprintf(L("tele.loop.teleconference_channel", "\n|09-=[ |15Teleconference|09 - channel |15%d|09 ]=-|07\n"), (int)chan);
        tputs(L("tele.loop.quit_who_me", "|08/q quit  /w who  /me action  /msg name text  /c n channel  /? help|07\n"));
        who_here();
    }
    tputs("|15> |07");
    while (N.online) {
        LONG k = tgetkey(0);
        if (k == KEY_HANGUP) break;
        if (k == KEY_NONE) {
            if (show_new(line, len)) { tputs(L("tele.loop.the_chat_has", "|08The chat has ended.|07\n")); break; }
            continue;
        }
        if (k == 8 || k == KEY_LEFT) {
            if (len > 0) { len--; tn_raw((const UBYTE *)"\b \b", 3); }
            continue;
        }
        if (k != '\r') {
            if (k >= 32 && k <= 255 && len < (LONG)sizeof(line) - 2) {
                UBYTE c = (UBYTE)k;
                line[len++] = (char)c;
                tputraw(&c, 1, CS_CP437);
            }
            continue;
        }
        line[len] = 0;
        tputs("\r");
        tcleol();
        if (len == 0) { tputs("|15> |07"); continue; }
        len = 0;
        if (line[0] == '/') {
            char cmd = line[1];
            char *arg = line + 2;
            if (cmd >= 'A' && cmd <= 'Z') cmd += 32;
            while (*arg && *arg != ' ') arg++;
            while (*arg == ' ') arg++;
            if (cmd == 'q') break;
            else if (cmd == 'w') who_here();
            else if (cmd == 'm' && !str_nicmp(line, "/me ", 4)) {
                tele_post(mychan, TK_ACTION, 0, line + 4);
                tprintf("|13* %s %s|07\n", N.user.name, line + 4);
            } else if (cmd == 'm' && !str_nicmp(line, "/msg ", 5)) {
                char name[NAMELEN], *sp = strchr(arg, ' ');
                int to;
                if (!sp) { tputs(L("tele.loop.msg_name_text", "|08/msg name text|07\n")); }
                else {
                    *sp = 0;
                    str_copy(name, arg, NAMELEN);
                    if (!(to = node_by_name(name))) tprintf(L("tele.loop.isnt_in_this", "|12%s isn't in this channel.|07\n"), name);
                    else {
                        tele_post(mychan, TK_WHISPER, (UBYTE)to, sp + 1);
                        tprintf(L("tele.loop.to", "|05(to %s)|08: |07%s\n"), name, sp + 1);
                    }
                }
            } else if (cmd == 'c' && !private) {
                int c = atoi(arg);
                if (c >= 1 && c < PRIVATE_BASE) {
                    leave_chan();
                    set_chan((UBYTE)c);
                    tprintf(L("tele.loop.now_in_channel", "|07Now in channel |15%d|07.\n"), c);
                    who_here();
                } else tputs(L("tele.loop.channels_are_1", "|08Channels are 1 to 99.|07\n"));
            } else {
                tputs(L("tele.loop.quit_whos_here", "|08/q quit  /w who's here  /me does something  /msg name text (whisper)"));
                if (!private) tputs(L("tele.loop.change_channel", "  /c n change channel"));
                tputs("|07\n");
            }
        } else {
            struct TeleLine me;
            memset(&me, 0, sizeof(me));
            me.fromnode = (UBYTE)N.node;
            me.kind = TK_SAY;
            str_copy(me.from, N.user.name, NAMELEN);
            str_copy(me.text, line, sizeof(me.text));
            tele_post(mychan, TK_SAY, 0, line);
            show_line(&me);
        }
        tputs("|15> |07");
    }
    if (N.online) {
        leave_chan();
        tputs("\n");
    }
    in_tele = FALSE;
    back_to_menu();
}

void teleconference(const char *arg)
{
    int c = (arg && *arg) ? atoi(arg) : 1;
    if (in_tele) return;
    if (c < 1 || c >= PRIVATE_BASE) c = 1;
    tele_loop((UBYTE)c);
}

/* ---- sysop chat --------------------------------------------------------------------- */

static int sysop_node(void)
{
    int n, found = 0;
    LONG slevel = cfg_int(N.cfg, "sysop_level", 255);
    struct UserRec u;
    shared_lock(N.S);
    for (n = 0; n < N.S->nodes && !found; n++) {
        struct NodeInfo *ni = &N.S->node[n];
        if (n + 1 == N.node || ni->state < NS_ONLINE || ni->userid <= 0) continue;
        shared_unlock(N.S);
        ObtainSemaphore(&N.S->userlock);
        if (userdb_read((ULONG)ni->userid, &u) && u.level >= slevel) found = n + 1;
        ReleaseSemaphore(&N.S->userlock);
        shared_lock(N.S);
    }
    shared_unlock(N.S);
    return found;
}

static BOOL send_chatreq(int to, const char *text)
{
    struct NodeInfo *ni;
    struct Task *t = NULL;
    shared_lock(N.S);
    ni = &N.S->node[to - 1];
    if (ni->state >= NS_ONLINE) {
        UBYTE next = (ni->msg_head + 1) % NODE_MSGQ;
        if (next != ni->msg_tail) {
            struct NodeMsg *m = &ni->msgq[ni->msg_head];
            m->from = (UBYTE)N.node;
            m->type = NM_CHATREQ;
            str_copy(m->fromname, N.user.name, NAMELEN);
            str_copy(m->text, text, sizeof(m->text));
            ni->msg_head = next;
            t = ni->task;
        }
    }
    shared_unlock(N.S);
    if (t) Signal(t, SIGBREAKF_CTRL_D);
    return t != NULL;
}

/* BBSControl is running: put the page on its screen and wait (up to 30 s, any key gives up) for
   Chat or Not now.  FALSE = BBSControl isn't there (or is busy with another page). */
static BOOL page_control(const char *why)
{
    struct Task *ctl;
    UBYTE ans = 0;
    int i;
    BOOL busy = FALSE;
    shared_lock(N.S);
    ctl = N.S->ctl_task;
    if (ctl && N.S->page_node && N.S->page_node != N.node) busy = TRUE;
    else if (ctl) {
        N.S->page_node = (UBYTE)N.node;
        N.S->page_answer = 0;
        N.S->page_seq++;
        str_copy(N.S->page_from, N.user.name, NAMELEN);
        str_copy(N.S->page_text, why, sizeof(N.S->page_text));
    }
    shared_unlock(N.S);
    if (!ctl) return FALSE;
    if (busy) {
        tputs(L("tele.page_control.the_sysop_is", "|14The sysop is answering another page - try again in a minute.|07\n"));
        return TRUE;
    }
    Signal(ctl, SIGBREAKF_CTRL_F);
    tputs(L("tele.page_control.paging_the_sysop", "|07Paging the sysop |08(any key gives up)|07 "));
    for (i = 0; i < 30 && N.online && !ans; i++) {
        LONG k = tgetkey(1);
        shared_lock(N.S);
        if (N.S->page_node == N.node) ans = N.S->page_answer;
        shared_unlock(N.S);
        if (ans) break;
        if (k == KEY_HANGUP || k >= 0) { ans = 3; break; }
        tputs("."); tn_flush();
    }
    if (!ans) ans = 3;
    shared_lock(N.S);
    if (N.S->page_node == N.node) {             /* withdraw it: BBSControl closes its alert */
        if (ans == 3) N.S->page_answer = 3;
        N.S->page_node = 0;
    }
    ctl = N.S->ctl_task;
    shared_unlock(N.S);
    if (ctl) Signal(ctl, SIGBREAKF_CTRL_F);
    if (!N.online) return TRUE;
    if (ans == 1) {
        bbs_log(BBS_SYSLOG, "node %d: the sysop answered %s's page at the console", N.node, N.user.name);
        tputs(L("tele.page_control.the_sysop_answers", "\n|14The sysop answers!|07\n"));
        tele_loop((UBYTE)(PRIVATE_BASE + N.node));
        return TRUE;
    }
    tputs(ans == 2 ? L("tele.page_control.the_sysop_cant", "\n|14The sysop can't chat just now.|07\n") : L("tele.page_control.the_sysop_isnt", "\n|14The sysop isn't answering.|07\n"));
    if (tyesno(L("tele.page_control.leave_them_message", "|07Leave them a message instead?"), TRUE))
        msg_post(cfg_str(N.cfg, "sysop_name", "Sysop"), "Page: chat request", 0);
    return TRUE;
}

/* a caller asks for the sysop */
void page_sysop(void)
{
    int sn = sysop_node();
    char why[100];
    tputs(L("tele.page_sysop.reason_for_the", "\n|07Reason for the page |08(Enter cancels)|07: |15"));
    if (tgetline(why, 70, 0) <= 0) return;
    bbs_log(BBS_SYSLOG, "node %d: %s paged the sysop: %s", N.node, N.user.name, why);
    if (sn && send_chatreq(sn, why)) {
        tputs(L("tele.page_sysop.the_sysop_has", "|10The sysop has been paged and will break in if they can.|07\n"));
        return;
    }
    if (page_control(why)) return;            /* BBSControl is open on the Amiga: it answered, or didn't */
    /* nobody on a node: beep the Amiga's screen, the old-fashioned way */
    tputs(L("tele.page_sysop.paging_the_sysop", "|07Paging the sysop"));
    {
        int i;
        for (i = 0; i < 5 && N.online; i++) {
            DisplayBeep(NULL);
            tputs(".");
            tn_flush();
            Delay(50);
        }
    }
    tputs(L("tele.page_sysop.the_sysop_isnt", "\n|14The sysop isn't answering.|07\n"));
    if (tyesno(L("tele.page_sysop.leave_them_message", "|07Leave them a message instead?"), TRUE))
        msg_post(cfg_str(N.cfg, "sysop_name", "Sysop"), "Page: chat request", 0);
}

/* sysop menu: break in on a node for a private chat */
void sysop_chat(void)
{
    char b[4];
    int n;
    char req[16];
    whos_online();
    tputs(L("tele.sysop_chat.chat_with_node", "\n|07Chat with node: |15"));
    if (tgetline(b, 3, GL_DIGITS) <= 0) return;
    n = atoi(b);
    if (n == N.node || n < 1 || n > N.S->nodes) return;
    sprintf(req, "#chat:%d", PRIVATE_BASE + n);
    if (!send_chatreq(n, req)) { tputs(L("tele.sysop_chat.nobody_there", "|12Nobody there.|07\n")); return; }
    tprintf(L("tele.sysop_chat.breaking_in_on", "|10Breaking in on node %d...|07\n"), n);
    tele_loop((UBYTE)(PRIVATE_BASE + n));
}

/* main menu: invite another caller to a private chat */
void caller_chat(void)
{
    char b[4], req[20], name[NAMELEN];
    int n, i, ok;
    whos_online();
    tputs(L("tele.caller_chat.chat_privately_with", "\n|07Chat privately with node |08(Enter cancels)|07: |15"));
    if (tgetline(b, 3, GL_DIGITS) <= 0) return;
    n = atoi(b);
    if (n == N.node) { tputs(L("tele.caller_chat.talking_to_yourself", "|08Talking to yourself?|07\n")); return; }
    name[0] = 0;
    shared_lock(N.S);
    ok = n >= 1 && n <= N.S->nodes;
    if (ok) {
        struct NodeInfo *ni = &N.S->node[n - 1];
        str_copy(name, ni->user, NAMELEN);
        if (ni->state == NS_DOOR) ok = -1;                     /* in a door: can't answer */
        else if (ni->state != NS_ONLINE || ni->userid <= 0) ok = 0;
        else if (!ni->available || ni->tele) ok = -2;          /* not taking chats, or already chatting */
    }
    shared_unlock(N.S);
    if (ok == 0) { tputs(L("tele.caller_chat.nobody_is_on", "|12Nobody is on that node.|07\n")); return; }
    if (ok == -1) { tprintf(L("tele.caller_chat.is_in_door", "|14%s is in a door game - try again when they're out.|07\n"), name); return; }
    if (ok == -2) { tprintf(L("tele.caller_chat.isnt_taking_chats", "|14%s isn't taking chats just now.|07\n"), name); return; }
    invite_chan = CALLER_BASE + N.node;
    invite_answer = 0;
    sprintf(req, "#invite:%d", invite_chan);
    if (!send_chatreq(n, req)) { tputs(L("tele.caller_chat.couldnt_reach_that", "|12Couldn't reach that node.|07\n")); invite_chan = 0; return; }
    bbs_log(BBS_SYSLOG, "node %d: %s invited %s (node %d) to a private chat", N.node, N.user.name, name, n);
    tprintf(L("tele.caller_chat.asking_any_key", "|07Asking |15%s|07 |08(any key gives up)|07 "), name);
    for (i = 0; i < 45 && N.online && !invite_answer; i++) {
        LONG k = tgetkey(1);                                  /* node messages (the answer) arrive here too */
        if (invite_answer) break;
        if (k == KEY_HANGUP || k >= 0) break;
        tputs("."); tn_flush();
    }
    if (invite_answer == 1) {
        invite_chan = 0;
        tprintf(L("tele.caller_chat.says_yes", "\n|10%s says yes!|07\n"), name);
        tele_loop((UBYTE)(CALLER_BASE + N.node));
        return;
    }
    if (invite_answer == 2) tprintf(L("tele.caller_chat.says_not_now", "\n|14%s says not now.|07\n"), name);
    else {
        sprintf(req, "#cancel:%d", invite_chan);             /* withdrawn: they get told if they answer later */
        send_chatreq(n, req);
        tprintf(L("tele.caller_chat.no_answer_from", "\n|14No answer from %s.|07\n"), name);
    }
    invite_chan = 0;
}

/* called from tcheck_messages() for NM_CHATREQ; TRUE if it was handled here */
BOOL tele_chat_request(struct NodeMsg *m)
{
    static int cancelled;
    if (!strncmp(m->text, "#invite:", 8)) {                 /* another caller asks us */
        int chan = atoi(m->text + 8);
        char req[20];
        if (in_tele || (N.user.flags & UF_NOPAGE) || invite_chan) {
            sprintf(req, "#decline:%d", chan);
            send_chatreq(m->from, req);
            return TRUE;
        }
        cancelled = 0;
        tprintf(L("tele.chat_request.node_asks_you", "\n\x07|14%s |07(node %d) |14asks you to chat privately.|07\n"), m->fromname, (int)m->from);
        if (tyesno(L("tele.chat_request.chat_with_them", "|07Chat with them?"), FALSE) && !cancelled && N.online) {
            sprintf(req, "#accept:%d", chan);
            send_chatreq(m->from, req);
            tele_loop((UBYTE)chan);
        } else {
            if (cancelled) tprintf(L("tele.chat_request.gave_up_waiting", "|08%s gave up waiting.|07\n"), m->fromname);
            sprintf(req, "#decline:%d", chan);
            send_chatreq(m->from, req);
        }
        back_to_menu();
        return TRUE;
    }
    if (!strncmp(m->text, "#accept:", 8) || !strncmp(m->text, "#decline:", 9)) {   /* the answer to ours */
        int chan = atoi(strchr(m->text, ':') + 1);
        if (invite_chan && chan == invite_chan) invite_answer = m->text[1] == 'a' ? 1 : 2;
        return TRUE;
    }
    if (!strncmp(m->text, "#cancel:", 8)) { cancelled = 1; return TRUE; }
    if (!strncmp(m->text, "#chat:", 6)) {
        int chan = atoi(m->text + 6);
        if (in_tele || chan < PRIVATE_BASE) return TRUE;
        tprintf(L("tele.chat_request.breaks_in_to", "\n\x07|14%s breaks in to chat with you!|07\n"), m->fromname);
        tele_loop((UBYTE)chan);
        return TRUE;
    }
    if (N.sysop) {
        tprintf(L("tele.chat_request.node_pages_the", "|14>> |15%s |07(node %d) |14pages the sysop:|07 %s\n"
                "|08   Sysop menu H breaks in to chat.|07\n"), m->fromname, (int)m->from, m->text);
        return TRUE;
    }
    return FALSE;
}
