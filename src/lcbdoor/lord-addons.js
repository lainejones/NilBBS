/*
 * lord-addons.js - additions to the JavaScript LORD remake for NilBBS.
 *
 * transpile.sh appends this file after LORD.js, so it shares LORD's globals
 * and leaves the upstream file untouched.  It adds:
 *
 *   1. A new day.  The remake never resets anything ("todo: until day change
 *      code implemented"), so forest fights, player fights, the Inn and Violet
 *      would stay used up forever.  Here the first visit on each calendar day
 *      gives a player fresh fights, full hit points, and brings them back
 *      from the dead - as LORD does.
 *   2. New places in (O)ther Places, written from scratch for NilBBS (they
 *      borrow the names of classic LORD add-ons, not their text or code):
 *        Forest Outhouse, The Gem Trader, Lets Go Fishing, Wheel, The Wise One,
 *        Dragon's Claw Tavern (ale, arm-wrestling, dice, a shared carving
 *        wall) and Castle Coldrake (a daily raid: gate, tower, barracks,
 *        and Lord Coldrake himself, with fights scaled to the player).
 *   Beaten in an add-on fight you're thrown out with 1 hit point and half
 *   your gold, rather than killed.
 *   3. Fixes: a Death Knight skill kill now pays out gold and experience;
 *      the title banner no longer says "FOR LASTCALLBBS".
 *
 * The places are screens: a draw function (LORD's `current') and an input
 * function, looked up in addon_screens by the onInput wrapper at the end.
 */

/* ---- settings -------------------------------------------------------------- */

FOREST_FIGHTS_PER_DAY = 20;            // the remake ships with 500 (a test value)

var ADDON_FISH_CASTS  = 10;            // fishing trips a day
var ADDON_WHEEL_SPINS = 3;             // spins of the wheel a day

/* ---- helpers ---------------------------------------------------------------- */

var addon_screens = [];
var addon_places  = {};                // (O)ther Places number -> start function

function addon_calendar_day() { return Math.floor(Date.now() / 86400000); }

/* The game day.  Nightly maintenance (onMaint, run by BBSMaint) starts each
 * day, as in LORD; while it keeps running, its day number rules, so a player
 * up past midnight doesn't get a second new day before maintenance.  If it
 * stops running, days follow the calendar. */
var addon_maint_day = null;
function addon_today()
{
	var cal = addon_calendar_day();
	if (addon_maint_day === null) {
		addon_maint_day = -1;
		if (typeof _bbs_load !== 'undefined' && _bbs_load()) {
			var d = _bbs_load_type('lord', -1, 'lord_maint_day');
			if (typeof d === 'number') addon_maint_day = d;
		}
	}
	return addon_maint_day >= cal - 1 ? addon_maint_day : cal;
}

function pv(key, def)
{
	var v = get_player_value(player, key);
	if (player_value_not_found == 1 || v === undefined || v === null) return def;
	return v;
}

function addon_header(title)
{
	writelog(' `%' + title);
	writelog('`2-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-═-');
}

function addon_rand(lo, hi) { return lo + getRandomInt(hi - lo + 1); }

function addon_screen(draw, input) { addon_screens.push({ draw: draw, input: input }); }

/* a result screen that returns to (O)ther Places on any key */
function addon_done()
{
	writelog('');
	writelog('`2(`0Any key`2 to go back)');
	current = addon_back;
}
function addon_back() { current = menu_other; draw_menu(1); }
addon_screen(addon_back, function (key, k) { addon_back(); });

/* a number typed at a prompt (LORD's buffer/status line); -1 until Enter */
function addon_number(key)
{
	if (key == 10) {
		var n = parseInt(buffer, 10);
		buffer = '';
		clearStatus();
		return isNaN(n) ? 0 : n;
	}
	if (key == 8) { buffer = buffer.substring(0, buffer.length - 1); clearStatus(); status = buffer; return -1; }
	if (key >= 48 && key <= 57 && buffer.length < 9) { buffer += fromChr(key); status = buffer; }
	return -1;
}

function addon_line(key)
{
	if (key == 10) { var s = buffer; buffer = ''; clearStatus(); return s; }
	if (key == 8) { buffer = buffer.substring(0, buffer.length - 1); clearStatus(); status = buffer; return null; }
	if (key >= 32 && key < 127 && buffer.length < 40) { buffer += fromChr(key); status = buffer; }
	return null;
}

/* ---- a new day -------------------------------------------------------------------- */

var addon_day_checked = '';

function addon_new_day()
{
	var today = addon_today();
	if (player == 'UNSET' || addon_day_checked == player) return;
	if (find_player(player) == -1) return;              // still signing up
	addon_day_checked = player;
	if (pv('lastday', -1) == today) return;
	addon_reset_player(players[find_player(player)], today);
	save_data();
}

/* a player's new day */
function addon_reset_player(p, today)
{
	p['fights']      = FOREST_FIGHTS_PER_DAY;
	p['pfights']     = HUMAN_FIGHTS_PER_DAY;
	p['hp']          = p['maxhp'];
	p['dead']        = 0;
	p['seen_master'] = 0;
	p['seenviolet']  = 0;
	p['seenbard']    = 0;
	p['flirted']     = 0;
	p['stayinn']     = 0;
	p['highspirits'] = 1;
	p['baraks_visited_today'] = 0;
	p['lastday']     = today;
}

/* ---- nightly maintenance: "LCBDoor LORD.js MAINT" (Doors.cfg maint = ...) ----------- */

function onMaint()
{
	var today = addon_calendar_day(), kept = [], gone = [], fresh = 0, i, p;
	load_savedata();
	for (i = 0; i < players.length; i++) {
		p = players[i];
		/* gone quiet for longer than LORD's grace period: deleted, as in LORD */
		if (typeof p['lastday'] === 'number' && today - p['lastday'] > DELETION_GRACE_PERIOD_DAYS) {
			gone.push(p['name']);
			continue;
		}
		if (p['lastday'] !== today) { addon_reset_player(p, today); fresh++; }
		kept.push(p);
	}
	players = kept;
	save_data();
	addon_shared_put('lord_maint_day', today);
	return 'LORD: a new day dawns - ' + fresh + ' warrior' + (fresh == 1 ? '' : 's') + ' refreshed, ' +
	       players.length + ' in the realm' +
	       (gone.length ? ', ' + gone.length + ' deleted after ' + DELETION_GRACE_PERIOD_DAYS +
	                      ' days away (' + gone.join(', ') + ')' : '') + '.';
}

/* ---- Forest Outhouse --------------------------------------------------------------- */

function addon_outhouse()
{
	addon_header('The Forest Outhouse');
	writelog('A lopsided wooden outhouse leans against an oak. A');
	writelog('crescent moon is carved in the door, and somebody has');
	writelog('scratched "Seth wuz here" beneath it.');
	writelog('');
	if (pv('igm_outhouse', -1) == addon_today()) {
		writelog('You were here earlier. Once a day is plenty.');
		addon_done();
		return;
	}
	writelog('(`0U`2)se the facilities');
	writelog('(`0P`2)eek down the hole');
	writelog('(`0L`2)eave it be');
	writelog('');
	writelog('Well? [`0L`2] : ');
}

function addon_outhouse_input(key, k)
{
	if (k != 'u' && k != 'p') { addon_back(); return; }
	set_player_value(player, 'igm_outhouse', addon_today());
	clearlog();
	addon_header('The Forest Outhouse');
	var r = getRandomInt(100);
	if (k == 'u') {
		if (r < 30) {
			writelog('You emerge a new warrior, light of step and ready');
			writelog('for battle. You gain `%2`2 forest fights!');
			inc_player_value(player, 'fights', 2, MAX_VALUE);
		} else if (r < 55) {
			var g = addon_rand(20, 60) * pv('level', 1);
			writelog('Tucked behind the loose board is a purse someone');
			writelog('forgot. It holds `%' + g + '`2 gold.');
			inc_player_value(player, 'gold', g, MAX_VALUE);
		} else if (r < 80) {
			writelog('A spider the size of your fist was waiting under the');
			writelog('seat. It bites, and you leave in a hurry.');
			var d = Math.max(1, Math.floor(pv('hp', 1) / 5));
			dec_player_value(player, 'hp', d, 1);
			writelog('You lose `4' + d + '`2 hit points.');
		} else {
			writelog('The door sticks. You are in there for an hour, and');
			writelog('everyone in town hears you yelling for help.');
			writelog('You lose `41`2 charm.');
			dec_player_value(player, 'charm', 1, 0);
		}
	} else {
		if (r < 20) {
			writelog('Something glints far below. You fish it out with a');
			writelog('stick - a `%gem`2! You try not to think about it.');
			inc_player_value(player, 'gems', 1, MAX_VALUE);
		} else {
			writelog('You lean in for a closer look. That was a mistake.');
			writelog('The smell follows you for the rest of the day.');
			writelog('You lose `41`2 charm.');
			dec_player_value(player, 'charm', 1, 0);
		}
	}
	addon_done();
}
addon_screen(addon_outhouse, addon_outhouse_input);

/* ---- The Gem Trader ----------------------------------------------------------------- */

var addon_trade_mode = '';

/* the day's price: it wanders between 600 and 1300 gold a gem */
function addon_gem_price() { return 600 + ((addon_today() * 7919) % 701); }

function addon_gems()
{
	var sell = addon_gem_price(), buy = Math.floor(sell * 5 / 4);
	addon_header('The Gem Trader');
	writelog('A thin man in a velvet coat weighs stones on a brass');
	writelog('scale. "Buying and selling, friend. Today\'s rates:"');
	writelog('');
	writelog('   I buy gems at  `%' + sell + '`2 gold each');
	writelog('   I sell gems at `%' + buy + '`2 gold each');
	writelog('');
	writelog('You carry `%' + pv('gems', 0) + '`2 gems and `%' + pv('gold', 0) + '`2 gold.');
	writelog('');
	writelog('(`0S`2)ell gems   (`0B`2)uy gems   (`0L`2)eave');
	writelog('');
	writelog('Your business? [`0L`2] : ');
	addon_trade_mode = '';
}

function addon_gems_input(key, k)
{
	if (addon_trade_mode == '') {
		if (k == 's') { addon_trade_mode = 's'; writelog('Sell how many? '); return; }
		if (k == 'b') { addon_trade_mode = 'b'; writelog('Buy how many? '); return; }
		addon_back();
		return;
	}
	var n = addon_number(key);
	if (n < 0) return;
	var sell = addon_gem_price(), buy = Math.floor(sell * 5 / 4);
	clearlog();
	addon_header('The Gem Trader');
	if (n == 0) writelog('"Browsing, then. Suit yourself."');
	else if (addon_trade_mode == 's') {
		if (n > pv('gems', 0)) writelog('"You don\'t have that many, friend."');
		else {
			dec_player_value(player, 'gems', n, 0);
			inc_player_value(player, 'gold', n * sell, MAX_VALUE);
			writelog('He counts out `%' + (n * sell) + '`2 gold for your ' + n + ' gem' + (n == 1 ? '.' : 's.'));
		}
	} else {
		if (n * buy > pv('gold', 0)) writelog('"That costs ' + (n * buy) + ' gold. Come back richer."');
		else {
			dec_player_value(player, 'gold', n * buy, 0);
			inc_player_value(player, 'gems', n, MAX_VALUE);
			writelog('You hand over `%' + (n * buy) + '`2 gold for ' + n + ' gem' + (n == 1 ? '.' : 's.'));
		}
	}
	addon_done();
}
addon_screen(addon_gems, addon_gems_input);

/* ---- Lets Go Fishing ------------------------------------------------------------------ */

function addon_fishing()
{
	var casts = pv('igm_fish_day', -1) == addon_today() ? pv('igm_fish_casts', 0) : 0;
	addon_header('Lets Go Fishing');
	writelog('A quiet lake lies past the edge of the forest. An old');
	writelog('man dozes on the dock beside a spare rod.');
	writelog('');
	writelog('Each cast costs a forest fight. You have `%' + pv('fights', 0) + '`2 left,');
	writelog('and the old man lets you cast `%' + (ADDON_FISH_CASTS - casts) + '`2 more times today.');
	writelog('');
	writelog('(`0C`2)ast a line   (`0L`2)eave');
	writelog('');
	writelog('Well? [`0C`2] : ');
}

function addon_fishing_input(key, k)
{
	if (k != 'c' && key != 10) { addon_back(); return; }
	var today = addon_today();
	if (pv('igm_fish_day', -1) != today) { set_player_value(player, 'igm_fish_day', today); set_player_value(player, 'igm_fish_casts', 0); }
	clearlog();
	addon_header('Lets Go Fishing');
	if (pv('igm_fish_casts', 0) >= ADDON_FISH_CASTS) {
		writelog('"That\'s enough for one day," the old man says, and');
		writelog('takes his rod back.');
		addon_done();
		return;
	}
	if (pv('fights', 0) <= 0) {
		writelog('You are too tired to fish. Come back tomorrow.');
		addon_done();
		return;
	}
	inc_player_value(player, 'igm_fish_casts', 1, MAX_VALUE);
	dec_player_value(player, 'fights', 1, 0);
	var lvl = pv('level', 1), r = getRandomInt(100);
	if (r < 45) {
		var g = addon_rand(15, 45) * lvl;
		writelog('A fat trout! The cook at the Inn pays `%' + g + '`2 gold.');
		inc_player_value(player, 'gold', g, MAX_VALUE);
	} else if (r < 60) {
		writelog('You reel in an old boot. It isn\'t even your size.');
	} else if (r < 70) {
		writelog('Your line snags on a sunken chest. Inside is a `%gem`2!');
		inc_player_value(player, 'gems', 1, MAX_VALUE);
	} else if (r < 80) {
		var x = lvl * addon_rand(5, 15);
		writelog('A talking carp tells you forest secrets in exchange');
		writelog('for its freedom. You gain `%' + x + '`2 experience.');
		inc_player_value(player, 'xp', x, MAX_XP);
	} else if (r < 85) {
		writelog('A silver ring glitters on the hook. It suits you.');
		writelog('You gain `%1`2 charm.');
		inc_player_value(player, 'charm', 1, MAX_VALUE);
	} else if (r < 95) {
		writelog('Nothing is biting. The old man snores on.');
	} else {
		var d = Math.max(1, Math.floor(pv('hp', 1) / 3));
		writelog('`4A lake serpent`2 bursts from the water and snaps at');
		writelog('you before you can scramble up the bank!');
		writelog('You lose `4' + d + '`2 hit points.');
		dec_player_value(player, 'hp', d, 1);
	}
	writelog('');
	writelog('(`0C`2)ast again   (`0L`2)eave [`0C`2] : ');
	current = addon_fishing_again;
}
function addon_fishing_again() { }
addon_screen(addon_fishing, addon_fishing_input);
addon_screen(addon_fishing_again, addon_fishing_input);

/* ---- Wheel ----------------------------------------------------------------------------- */

/* the wheel's twelve wedges */
var addon_wedges = ['BANKRUPT', 'x2', 'LOSE', 'x3', 'LOSE', 'GEM', 'x2', 'LOSE', 'FIGHT', 'LOSE', 'x5', 'LOSE'];

function addon_wheel()
{
	var spins = pv('igm_wheel_day', -1) == addon_today() ? pv('igm_wheel_spins', 0) : 0;
	addon_header('The Wheel');
	writelog('A painted wheel taller than a man turns in the town');
	writelog('square. A barker in a striped vest waves you over.');
	writelog('');
	writelog('"Name your bet! Double it, triple it, five times it!');
	writelog(' Or win a gem, or a fight\'s worth of energy!"');
	writelog('');
	writelog('You have `%' + pv('gold', 0) + '`2 gold and `%' + (ADDON_WHEEL_SPINS - spins) + '`2 spins left today.');
	writelog('');
	if (spins >= ADDON_WHEEL_SPINS) {
		writelog('"Come back tomorrow, friend! The wheel needs a rest."');
		addon_done();
		return;
	}
	writelog('Your bet (`00`2 to leave) : ');
}

function addon_wheel_input(key, k)
{
	var bet = addon_number(key);
	if (bet < 0) return;
	if (bet == 0) { addon_back(); return; }
	clearlog();
	addon_header('The Wheel');
	if (bet > pv('gold', 0)) { writelog('"You don\'t have that much, friend!"'); addon_done(); return; }
	var today = addon_today();
	if (pv('igm_wheel_day', -1) != today) { set_player_value(player, 'igm_wheel_day', today); set_player_value(player, 'igm_wheel_spins', 0); }
	inc_player_value(player, 'igm_wheel_spins', 1, MAX_VALUE);
	var w = addon_wedges[getRandomInt(addon_wedges.length)];
	writelog('The wheel spins... clack clack clack... clack...');
	writelog('');
	writelog('It stops on `%' + w + '`2!');
	writelog('');
	if (w == 'x2' || w == 'x3' || w == 'x5') {
		var m = parseInt(w.substring(1), 10), won = bet * (m - 1);
		inc_player_value(player, 'gold', won, MAX_VALUE);
		writelog('You win `%' + won + '`2 gold!');
		if (m == 5 && bet >= 1000) add_news('`0' + player + '`2 hit the five on the Wheel and won `%' + won + '`2 gold!');
	} else if (w == 'GEM') {
		inc_player_value(player, 'gems', 1, MAX_VALUE);
		writelog('You keep your bet and win a `%gem`2!');
	} else if (w == 'FIGHT') {
		inc_player_value(player, 'fights', 1, MAX_VALUE);
		writelog('You keep your bet, and the crowd\'s cheering gives');
		writelog('you the energy for `%1`2 more forest fight!');
	} else if (w == 'BANKRUPT') {
		var lost = Math.min(pv('gold', 0), bet * 2);
		dec_player_value(player, 'gold', lost, 0);
		writelog('`4BANKRUPT!`2 The barker takes `4' + lost + '`2 gold - double');
		writelog('your bet. "Rules are rules, friend."');
	} else {
		dec_player_value(player, 'gold', bet, 0);
		writelog('You lose your `4' + bet + '`2 gold.');
	}
	addon_done();
}
addon_screen(addon_wheel, addon_wheel_input);

/* ---- The Wise One ---------------------------------------------------------------------- */

/* folk riddles; any of the words after the question counts as right */
var addon_riddles = [
	['What has keys but cannot open a single lock?', 'piano', 'keyboard'],
	['The more you take, the more you leave behind. What?', 'footstep', 'steps', 'footprint'],
	['What runs but never walks, has a bed but never sleeps?', 'river', 'stream'],
	['What has a neck but no head?', 'bottle'],
	['What can you catch but never throw?', 'cold'],
	['What gets wetter the more it dries?', 'towel'],
	['What has one eye but cannot see?', 'needle'],
	['What belongs to you, yet others use it more?', 'name'],
	['What has hands but cannot clap?', 'clock'],
	['Feed me and I live, give me drink and I die.', 'fire', 'flame'],
	['What goes up but never comes down?', 'age'],
	['What has many teeth but cannot bite?', 'comb', 'saw']
];
var addon_riddle = 0;

function addon_wise()
{
	addon_header('The Wise One');
	writelog('Deep in the forest, in a hut of woven willow, sits a');
	writelog('woman older than the trees around her.');
	writelog('');
	if (pv('igm_wise', -1) == addon_today()) {
		writelog('"One riddle a day, child. Wisdom must be earned."');
		addon_done();
		return;
	}
	/* a different riddle per player per day, so a wrong answer spoils nothing */
	addon_riddle = (addon_today() + find_player(player) * 5) % addon_riddles.length;
	writelog('"Answer my riddle and I shall teach you something."');
	writelog('');
	writelog('`0' + addon_riddles[addon_riddle][0]);
	writelog('');
	writelog('Your answer (one word) : ');
}

function addon_wise_input(key, k)
{
	var a = addon_line(key);
	if (a === null) return;
	set_player_value(player, 'igm_wise', addon_today());
	clearlog();
	addon_header('The Wise One');
	a = a.toLowerCase();
	var right = false, i, r = addon_riddles[addon_riddle];
	for (i = 1; i < r.length; i++) if (a.indexOf(r[i]) != -1) right = true;
	if (right) {
		var x = pv('level', 1) * 30;
		writelog('She smiles, and for a moment looks young again.');
		writelog('"Well reasoned." You gain `%' + x + '`2 experience and `%1`2 charm.');
		inc_player_value(player, 'xp', x, MAX_XP);
		inc_player_value(player, 'charm', 1, MAX_VALUE);
	} else {
		writelog('"No." She pokes the fire. "The answer was `0' + r[1] + '`2."');
		writelog('"Come back tomorrow, and think harder."');
	}
	addon_done();
}
addon_screen(addon_wise, addon_wise_input);

/* ---- shared data (every player sees it): kept in the door's store ----------------------- */

function addon_shared_get(key)
{
	if (typeof _bbs_load === 'undefined') return [];
	_bbs_load();
	var v = _bbs_load_type('lord', [], key);
	return v ? v : [];
}

function addon_shared_put(key, v)
{
	if (typeof _bbs_save_type === 'undefined') return;
	_bbs_save_type('lord', key, v);
	_bbs_save();
}

/* per-day counters on the player: addon_count('igm_claw_drinks') */
function addon_count(key)
{
	return pv(key + '_day', -1) == addon_today() ? pv(key, 0) : 0;
}
function addon_bump(key)
{
	var n = addon_count(key) + 1;
	set_player_value(player, key + '_day', addon_today());
	set_player_value(player, key, n);
	return n;
}

/* ---- a fight, for places that have monsters ---------------------------------------------
 * Monsters are scaled to the player: `rounds' is about how many good hits
 * kill it, `bite' the share of the player's max hit points it takes a round.
 * m = { name, weapon, hp, str, win: fn, lose: fn } */

var addon_mon = null;

function addon_monster(name, weapon, rounds, bite, win)
{
	var str = pv('str', 10), def = pv('def', 1), maxhp = pv('maxhp', 20);
	return {
		name: name, weapon: weapon,
		hp: Math.max(3, Math.floor(rounds * str * 0.75 * (0.85 + Math.random() * 0.3))),
		str: Math.max(2, Math.floor((maxhp * bite + def) / 0.75)),
		win: win
	};
}

function addon_fight_status()
{
	writelog('');
	writelog('Your hitpoints : `0' + pv('hp', 1));
	writelog(addon_mon.name + '\'s hitpoints : `0' + addon_mon.hp);
	writelog('');
	writelog('(`0A`2)ttack   (`0R`2)un');
	writelog('Your command? [`0A`2] : ');
}

function addon_fight_start(m, intro)
{
	addon_mon = m;
	clearlog();
	addon_header('**FIGHT**');
	intro.split('\n').forEach(function (l) { writelog(l); });
	addon_fight_status();
	current = addon_fight;
}

function addon_fight() { }

function addon_cap(s) { return s.charAt(0).toUpperCase() + s.substring(1); }

/* beaten: in LORD you would die; here you are thrown out with half your gold */
function addon_knocked_out()
{
	var lost = Math.floor(pv('gold', 0) / 2);
	set_player_value(player, 'hp', 1);
	dec_player_value(player, 'gold', lost, 0);
	writelog('');
	writelog('`4Everything goes black.`2');
	writelog('You wake up in a ditch outside with `41`2 hit point.');
	if (lost) writelog('Someone has helped themselves to `4' + lost + '`2 gold.');
	addon_done();
}

function addon_fight_input(key, k)
{
	var m = addon_mon, str = pv('str', 10), def = pv('def', 1), hit, bite;
	clearlog();
	addon_header('**FIGHT**');
	if (k == 'r') {
		if (getRandomInt(100) < 50) {
			writelog('You turn tail and run - and get away!');
			addon_done();
			return;
		}
		writelog('You try to run, but ' + m.name + ' cuts you off!');
	} else {
		hit = addon_rand(Math.floor(str / 2), str);
		if (hit <= 0) writelog('You miss ' + m.name + ' completely!');
		else {
			m.hp -= hit;
			writelog('You hit ' + m.name + ' for `0' + hit + '`2 damage!');
		}
		if (m.hp <= 0) {
			writelog('');
			writelog('`0You have defeated ' + m.name + '!`2');
			m.win();
			return;
		}
	}
	bite = addon_rand(Math.floor(m.str / 2), m.str) - def;
	if (bite <= 0) writelog(addon_cap(m.name) + ' misses you.');
	else {
		writelog(addon_cap(m.name) + ' hits you for `4' + bite + '`2 damage!');
		if (pv('hp', 1) - bite <= 0) { set_player_value(player, 'hp', 0); addon_knocked_out(); return; }
		dec_player_value(player, 'hp', bite, 0);
	}
	addon_fight_status();
}
addon_screen(addon_fight, addon_fight_input);

/* a reward line helper */
function addon_reward(gold, xp, gems)
{
	if (gold) { inc_player_value(player, 'gold', gold, MAX_VALUE); writelog('You find `%' + gold + '`2 gold.'); }
	if (xp) { inc_player_value(player, 'xp', xp, MAX_XP); writelog('You gain `%' + xp + '`2 experience.'); }
	if (gems) { inc_player_value(player, 'gems', gems, MAX_VALUE); writelog('You find `%' + gems + '`2 gem' + (gems == 1 ? '!' : 's!')); }
}

/* ---- Dragon's Claw Tavern --------------------------------------------------------------- */

var CLAW_DRINKS = 6, CLAW_BONES = 5, CLAW_ALE = 20;
var claw_mode = '';

function addon_claw()
{
	var carv = addon_shared_get('igm_claw_carvings'), i;
	addon_header('Dragon\'s Claw Tavern');
	writelog('Smoke, sawdust and a great black claw nailed over the');
	writelog('bar. A one-eyed half-orc named Grog polishes a mug and');
	writelog('watches you come in.');
	if (carv.length) {
		writelog('Carved in the bar, freshest first:');
		for (i = carv.length - 1; i >= 0 && i >= carv.length - 2; i--) writelog('  `0' + carv[i]);
	}
	writelog('');
	writelog('(`0D`2)rink an ale (' + CLAW_ALE + ' gold)   (`0A`2)rm-wrestle Grog');
	writelog('(`0B`2)ones - dice with a stranger    (`0C`2)arve the bar');
	writelog('(`0L`2)eave');
	writelog('');
	writelog('Well? [`0L`2] : ');
	claw_mode = '';
}

function addon_claw_input(key, k)
{
	if (claw_mode == 'a' || claw_mode == 'b') { addon_claw_bet(key); return; }
	if (claw_mode == 'c') { addon_claw_carve(key); return; }
	if (k == 'd') { addon_claw_drink(); return; }
	if (k == 'a') {
		if (addon_count('igm_claw_wrestle') >= 1) {
			clearlog(); addon_header('Dragon\'s Claw Tavern');
			writelog('Grog is still rubbing his arm. "Tomorrow," he growls.');
			addon_done();
			return;
		}
		claw_mode = 'a';
		writelog('"Put your gold on the bar, then." How much? ');
		return;
	}
	if (k == 'b') {
		if (addon_count('igm_claw_bones') >= CLAW_BONES) {
			clearlog(); addon_header('Dragon\'s Claw Tavern');
			writelog('The stranger pockets his dice. "Enough for today."');
			addon_done();
			return;
		}
		claw_mode = 'b';
		writelog('"Two bones each, high roll wins. Your stake?" ');
		return;
	}
	if (k == 'c') {
		if (addon_count('igm_claw_carve') >= 1) {
			clearlog(); addon_header('Dragon\'s Claw Tavern');
			writelog('Grog eyes your knife. "One carving a day, friend."');
			addon_done();
			return;
		}
		claw_mode = 'c';
		writelog('Carve what? (up to 40 letters) ');
		return;
	}
	addon_back();
}

function addon_claw_drink()
{
	clearlog();
	addon_header('Dragon\'s Claw Tavern');
	if (addon_count('igm_claw_drinks') >= CLAW_DRINKS) {
		writelog('Grog takes the mug out of your hand. "You\'ve had');
		writelog('enough. Go home before you fall over."');
		addon_done();
		return;
	}
	if (pv('gold', 0) < CLAW_ALE) { writelog('"Ale costs gold, friend." You don\'t have ' + CLAW_ALE + '.'); addon_done(); return; }
	dec_player_value(player, 'gold', CLAW_ALE, 0);
	var n = addon_bump('igm_claw_drinks'), r = getRandomInt(100);
	writelog('Grog slides a foaming mug down the bar. That\'s drink');
	writelog('number `0' + n + '`2 today.');
	writelog('');
	if (n == 1) {
		writelog('The ale warms your belly and steels your nerve.');
		writelog('You gain `%1`2 forest fight!');
		inc_player_value(player, 'fights', 1, MAX_VALUE);
	} else if (n <= 3) {
		if (r < 50) {
			writelog('You tell the tale of your last battle, and the whole');
			writelog('room laughs in the right places. You gain `%1`2 charm.');
			inc_player_value(player, 'charm', 1, MAX_VALUE);
		} else { writelog('A bard in the corner plays a song about the Red'); writelog('Dragon. You hum along badly.'); }
	} else if (r < 40) {
		var d = Math.max(1, Math.floor(pv('hp', 1) / 5));
		writelog('You spill a drink on a very large dwarf. The brawl');
		writelog('that follows costs you `4' + d + '`2 hit points.');
		dec_player_value(player, 'hp', d, 1);
	} else if (r < 70) {
		var lost = Math.floor(pv('gold', 0) / 10);
		writelog('You doze off on the bar. When you wake up, your purse');
		writelog('is lighter by `4' + lost + '`2 gold.');
		dec_player_value(player, 'gold', lost, 0);
	} else writelog('The room is spinning, but pleasantly.');
	addon_done();
}

function addon_claw_bet(key)
{
	var bet = addon_number(key), mode = claw_mode;
	if (bet < 0) return;
	claw_mode = '';
	clearlog();
	addon_header('Dragon\'s Claw Tavern');
	if (bet <= 0) { writelog('You think better of it.'); addon_done(); return; }
	if (bet > pv('gold', 0)) { writelog('"You don\'t have that, friend."'); addon_done(); return; }
	if (mode == 'a') {
		/* Grog is strong for your level; the stronger you are, the better your odds */
		var lvl = pv('level', 1), str = pv('str', 10), grog = 12 + lvl * 9;
		var cap = 100 * lvl;
		if (bet > cap) { bet = cap; writelog('"I don\'t take more than ' + cap + ' from the likes of you."'); }
		addon_bump('igm_claw_wrestle');
		writelog('You lock hands with Grog. The crowd gathers...');
		writelog('');
		if (Math.random() < str / (str + grog)) {
			writelog('Veins stand out on his neck - and his hand slams');
			writelog('down on the bar! `0You win`2 `%' + bet + '`2 gold!');
			inc_player_value(player, 'gold', bet, MAX_VALUE);
			if (bet >= 500) add_news('`0' + player + '`2 beat Grog at arm-wrestling in the Dragon\'s Claw!');
		} else {
			writelog('Grog grins, and slowly, slowly, your arm goes over.');
			writelog('`4You lose`2 your ' + bet + ' gold.');
			dec_player_value(player, 'gold', bet, 0);
		}
	} else {
		var a = addon_rand(1, 6), b = addon_rand(1, 6), c = addon_rand(1, 6), d = addon_rand(1, 6);
		addon_bump('igm_claw_bones');
		writelog('You roll `0' + a + '`2 and `0' + b + '`2 - that\'s `%' + (a + b) + '`2.');
		writelog('The stranger rolls `0' + c + '`2 and `0' + d + '`2 - that\'s `%' + (c + d) + '`2.');
		writelog('');
		if (a + b > c + d) { writelog('`0You win`2 `%' + bet + '`2 gold!'); inc_player_value(player, 'gold', bet, MAX_VALUE); }
		else if (a + b < c + d) { writelog('`4You lose`2 your ' + bet + ' gold.'); dec_player_value(player, 'gold', bet, 0); }
		else writelog('A tie. You both pick up your stakes.');
	}
	addon_done();
}

function addon_claw_carve(key)
{
	var s = addon_line(key), carv, i;
	if (s === null) return;
	claw_mode = '';
	clearlog();
	addon_header('Dragon\'s Claw Tavern');
	s = s.replace(/`/g, '\'');
	if (!s.replace(/ /g, '').length) { writelog('You put the knife away.'); addon_done(); return; }
	addon_bump('igm_claw_carve');
	carv = addon_shared_get('igm_claw_carvings');
	carv.push(player + ': ' + s);
	while (carv.length > 12) carv.shift();
	addon_shared_put('igm_claw_carvings', carv);
	writelog('You carve your words into the bar. Grog pretends not');
	writelog('to notice. The bar reads:');
	writelog('');
	for (i = carv.length - 1; i >= 0 && i >= carv.length - 8; i--) writelog('  `0' + carv[i]);
	addon_done();
}
addon_screen(addon_claw, addon_claw_input);

/* ---- Castle Coldrake ------------------------------------------------------------------------ */

var castle_tower = 0, castle_barracks = 0;

function addon_castle()
{
	addon_header('Castle Coldrake');
	writelog('Black towers rise out of the mist on a crag above the');
	writelog('forest. The ice-cold wind smells of old stone. A guard');
	writelog('in rust-red armour blocks the gate.');
	writelog('');
	if (pv('igm_castle', -1) == addon_today()) {
		writelog('The guard recognises you and lowers his halberd.');
		writelog('"Once a day is more than enough. Be off."');
		addon_done();
		return;
	}
	writelog('"State your business at Castle Coldrake."');
	writelog('');
	writelog('(`0F`2)ight your way in');
	writelog('(`0B`2)ribe the guard (`%' + addon_bribe() + '`2 gold)');
	writelog('(`0L`2)eave');
	writelog('');
	writelog('You decide to... [`0L`2] : ');
}

function addon_bribe() { return 100 * pv('level', 1); }

function addon_castle_input(key, k)
{
	if (k != 'f' && k != 'b') { addon_back(); return; }
	set_player_value(player, 'igm_castle', addon_today());
	castle_tower = castle_barracks = 0;
	if (k == 'b' && pv('gold', 0) >= addon_bribe()) {
		dec_player_value(player, 'gold', addon_bribe(), 0);
		clearlog();
		addon_header('Castle Coldrake');
		writelog('The gold vanishes into the guard\'s gauntlet. He looks');
		writelog('the other way as you slip past.');
		writelog('');
		writelog('(`0Any key`2 to go in)');
		current = addon_castle_go_in;
		return;
	}
	addon_fight_start(addon_monster('the Gate Guard', 'his halberd', 2, 0.05, function () {
		addon_reward(20 * pv('level', 1), 5 * pv('level', 1), 0);
		writelog('');
		writelog('(`0Any key`2 to go in)');
		current = addon_castle_go_in;
	}), k == 'b' ? 'The guard laughs at your purse and raises his halberd!'
	             : 'You draw your weapon on the Gate Guard!');
}
addon_screen(addon_castle, addon_castle_input);

function addon_castle_go_in() { }
addon_screen(addon_castle_go_in, function () { current = addon_courtyard; draw_menu(1); });

function addon_courtyard()
{
	addon_header('Castle Coldrake - the Courtyard');
	writelog('Frost glitters on the flagstones. Banners with a white');
	writelog('drake on red hang still in the cold.');
	writelog('');
	writelog('(`0W`2)est tower' + (castle_tower ? '   `8(searched)`2' : ''));
	writelog('(`0B`2)arracks' + (castle_barracks ? '     `8(cleared)`2' : ''));
	writelog('(`0K`2)eep - where Lord Coldrake waits');
	writelog('(`0L`2)eave the castle');
	writelog('');
	writelog('HP: (`0' + pv('hp', 1) + '`2 of `0' + pv('maxhp', 20) + '`2)   Gold: `0' + pv('gold', 0) + '`2   Gems: `0' + pv('gems', 0));
	writelog('');
	writelog('Where to? : ');
}

function addon_courtyard_input(key, k)
{
	var lvl = pv('level', 1);
	if (k == 'w' && !castle_tower) {
		var r = getRandomInt(100);
		castle_tower = 1;
		clearlog();
		addon_header('Castle Coldrake - the West Tower');
		writelog('You climb a spiral stair slick with ice...');
		writelog('');
		if (r < 40) {
			writelog('At the top is an old sea chest. Inside, wrapped in');
			writelog('oilcloth:');
			addon_reward(0, 0, 1 + getRandomInt(2));
		} else if (r < 70) {
			var d = Math.max(1, Math.floor(pv('maxhp', 20) / 5));
			writelog('A step gives way under you! You tumble half the');
			writelog('stair and lose `4' + Math.min(d, pv('hp', 1) - 1) + '`2 hit points.');
			dec_player_value(player, 'hp', d, 1);
		} else {
			writelog('A wizard\'s study, long abandoned. You read until the');
			writelog('candle gutters out.');
			addon_reward(0, 15 * lvl, 0);
		}
		writelog('');
		writelog('(`0Any key`2 to go back down)');
		current = addon_castle_go_in;
		return;
	}
	if (k == 'b' && !castle_barracks) {
		addon_fight_start(addon_monster('a Coldrake Soldier', 'a notched sword', 3, 0.07, function () {
			addon_reward(40 * lvl, 10 * lvl, 0);
			writelog('');
			writelog('A sergeant bellows and charges in! (`0Any key`2)');
			current = addon_castle_sergeant;
		}), 'A soldier looks up from his dice and grabs his sword!');
		return;
	}
	if (k == 'k') {
		addon_fight_start(addon_monster('Lord Coldrake', 'Frostbite, his rune sword', 7, 0.11, function () {
			addon_reward(250 * lvl, 60 * lvl, 2);
			add_news('`0' + player + '`2 stormed `%Castle Coldrake`2 and defeated Lord Coldrake!',
			         '"The cold take you!" he screams as he falls.');
			writelog('');
			writelog('The castle falls silent. You walk out through the');
			writelog('gate as a legend.');
			addon_done();
		}), 'Lord Coldrake rises from an ice-white throne and\ndraws Frostbite, a blade that smokes with cold.\n"Another fool come to die." he sighs.');
		return;
	}
	if (k == 'l') { addon_back(); return; }
}
addon_screen(addon_courtyard, addon_courtyard_input);

function addon_castle_sergeant() { }
addon_screen(addon_castle_sergeant, function () {
	var lvl = pv('level', 1);
	addon_fight_start(addon_monster('the Sergeant', 'a spiked mace', 4, 0.09, function () {
		castle_barracks = 1;
		addon_reward(60 * lvl, 15 * lvl, 1);
		writelog('');
		writelog('The barracks are yours. (`0Any key`2)');
		current = addon_castle_go_in;
	}), 'The Sergeant swings a spiked mace at your head!');
});

/* ---- fixes to the remake ------------------------------------------------------------------ */

/* A Death Knight skill attack that kills took the monster below 0 hit points
   but never called perform_monster_died: no gold, no experience, and the
   fight just ended (onInput sees monster_hp <= 0 and goes back to the forest). */
var addon_base_perform_skill = perform_skill;
perform_skill = function ()
{
	var before = monster_hp;
	addon_base_perform_skill();
	if (before > 0 && monster_hp <= 0) {
		if (text_buffer[text_buffer.length - 1] == '<MORE>') {   /* one <MORE>, after the reward */
			text_buffer.pop(); text_lines--;
		}
		perform_monster_died(player, monster, monster_death, monster_xp, monster_gp,
		                     getRandomInt(100) >= 90);
	}
};

/* the title banner said "FOR LASTCALLBBS"; the author credit line stays */
(function () {
	for (var i = 0; i < banner.length; i++)
		if (banner[i].indexOf('LASTCALLBBS') != -1) {
			var s = 'FOR THE COMMODORE AMIGA', w = 54, l = Math.floor((w - s.length) / 2);
			banner[i] = '=║' + Array(l + 1).join(' ') + s + Array(w - l - s.length + 1).join(' ') + '║';
		}
})();

/* ---- hooking in ---------------------------------------------------------------------------- */

/* (O)ther Places numbers are positions in LORD's igms[] list, counted from 1 */
function addon_place(name, start)
{
	var i = igms.indexOf(name);
	if (i == -1) return;
	addon_places[i + 1] = start;
	enabled_igms.push(i);                       // highlighted in the menu
}
addon_place('ForestOuthouse',  addon_outhouse);
addon_place('The Gem Trader',  addon_gems);
addon_place('Lets Go Fishing', addon_fishing);
addon_place('Wheel',           addon_wheel);
addon_place('The Wise One',    addon_wise);
addon_place('DragonsClawTavern', addon_claw);
addon_place('Castle Coldrake', addon_castle);

var addon_base_onInput = onInput;

onInput = function (key)
{
	var i, k = fromChr(key).toLowerCase();
	if (!show_banner) addon_new_day();

	/* a place's own screens */
	for (i = 0; i < addon_screens.length; i++)
		if (current == addon_screens[i].draw) { addon_screens[i].input(key, k); return; }

	/* picking one of ours in (O)ther Places */
	if (current == menu_other && key == 10 && addon_places[parseInt(buffer, 10)]) {
		var start = addon_places[parseInt(buffer, 10)];
		buffer = '';
		clearStatus();
		other_menu_index = 0;
		current = start;
		draw_menu(1);
		return;
	}
	addon_base_onInput(key);
};
