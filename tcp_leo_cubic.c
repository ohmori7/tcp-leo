// SPDX-License-Identifier: GPL-2.0-only
/*
 * TCP CUBIC: Binary Increase Congestion control for TCP v2.3
 * Home page:
 *      http://netsrv.csc.ncsu.edu/twiki/bin/view/Main/BIC
 * This is from the implementation of CUBIC TCP in
 * Sangtae Ha, Injong Rhee and Lisong Xu,
 *  "CUBIC: A New TCP-Friendly High-Speed TCP Variant"
 *  in ACM SIGOPS Operating System Review, July 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/cubic_a_new_tcp_2008.pdf
 *
 * CUBIC integrates a new slow start algorithm, called HyStart.
 * The details of HyStart are presented in
 *  Sangtae Ha and Injong Rhee,
 *  "Taming the Elephants: New TCP Slow Start", NCSU TechReport 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/hystart_techreport_2008.pdf
 *
 * All testing results are available from:
 * http://netsrv.csc.ncsu.edu/wiki/index.php/TCP_Testing
 *
 * Unless CUBIC is enabled and congestion window is large
 * this behaves the same as the original Reno.
 */

#include <linux/mm.h>
#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <linux/timekeeping.h>		/* starlink time */
#include <net/tcp.h>

#define STARLINK_DEBUG
#ifdef STARLINK_DEBUG
#define DP(...)	printk(__VA_ARGS__)
#else /* STARLINK_DEBUG */
#define DP(...)
#endif /* ! STARLINK_DEBUG */

#define STARLINK_HANDOVER

#define BICTCP_BETA_SCALE    1024	/* Scale factor beta calculation
					 * max_cwnd = snd_cwnd * beta
					 */
#define	BICTCP_HZ		10	/* BIC HZ 2^10 = 1024 */

/* Two methods of hybrid slow start */
#define HYSTART_ACK_TRAIN	0x1
#define HYSTART_DELAY		0x2

/* Number of delay samples for detecting the increase of delay */
#define HYSTART_MIN_SAMPLES	8
#define HYSTART_DELAY_MIN	(4000U)	/* 4 ms */
#define HYSTART_DELAY_MAX	(16000U)	/* 16 ms */
#define HYSTART_DELAY_THRESH(x)	clamp(x, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)

static int fast_convergence __read_mostly = 1;
static int beta __read_mostly = 717;	/* = 717/1024 (BICTCP_BETA_SCALE) */
static int initial_ssthresh __read_mostly;
static int bic_scale __read_mostly = 41;
static int tcp_friendliness __read_mostly = 1;

static int hystart __read_mostly = 1;
static int hystart_detect __read_mostly = HYSTART_ACK_TRAIN | HYSTART_DELAY;
static int hystart_low_window __read_mostly = 16;
static int hystart_ack_delta_us __read_mostly = 2000;

static u32 cube_rtt_scale __read_mostly;
static u32 beta_scale __read_mostly;
static u64 cube_factor __read_mostly;

#ifdef STARLINK_HANDOVER
static s64 starlink_jiffies_base __read_mostly;
#define STARLINK_HANDOVER_OFFSET_DEFAULT	(200ULL)
#define STARLINK_HANDOVER_OFFSET_MAX		(1000ULL)
#define __STARLINK_HANDOVER_OFFSET(v)					\
	((u64)(v) <= STARLINK_HANDOVER_OFFSET_MAX ?			\
	 (u64)(v) : STARLINK_HANDOVER_OFFSET_MAX)
#define STARLINK_HANDOVER_OFFSET_START					\
	__STARLINK_HANDOVER_OFFSET(starlink_handover_start_ms)
#define STARLINK_HANDOVER_OFFSET_END					\
	__STARLINK_HANDOVER_OFFSET(starlink_handover_end_ms)
static int starlink_handover_start_ms __read_mostly =
    STARLINK_HANDOVER_OFFSET_DEFAULT;
static int starlink_handover_end_ms __read_mostly =
    STARLINK_HANDOVER_OFFSET_DEFAULT;
static struct hrtimer starlink_jiffies_sync_timer;
#endif /* STARLINK_HANDOVER */

/* Note parameters that are used for precomputing scale factors are read-only */
module_param(fast_convergence, int, 0644);
MODULE_PARM_DESC(fast_convergence, "turn on/off fast convergence");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
module_param(bic_scale, int, 0444);
MODULE_PARM_DESC(bic_scale, "scale (scaled by 1024) value for bic function (bic_scale/1024)");
module_param(tcp_friendliness, int, 0644);
MODULE_PARM_DESC(tcp_friendliness, "turn on/off tcp friendliness");
module_param(hystart, int, 0644);
MODULE_PARM_DESC(hystart, "turn on/off hybrid slow start algorithm");
module_param(hystart_detect, int, 0644);
MODULE_PARM_DESC(hystart_detect, "hybrid slow start detection mechanisms"
		 " 1: packet-train 2: delay 3: both packet-train and delay");
module_param(hystart_low_window, int, 0644);
MODULE_PARM_DESC(hystart_low_window, "lower bound cwnd for hybrid slow start");
module_param(hystart_ack_delta_us, int, 0644);
MODULE_PARM_DESC(hystart_ack_delta_us, "spacing between ack's indicating train (usecs)");

#ifdef STARLINK_HANDOVER
module_param(starlink_handover_start_ms, int, 0644);
MODULE_PARM_DESC(starlink_handover_start_ms, "starting offset of handover (0<=offset<=1000)");
module_param(starlink_handover_end_ms, int, 0644);
MODULE_PARM_DESC(starlink_handover_end_ms, "ending offset of handover (0<=offset<=1000)");
#endif /* STARLINK_HANDOVER */

/* BIC TCP Parameters */
struct bictcp {
	u32	cnt;		/* increase cwnd by 1 after ACKs */
	u32	last_max_cwnd;	/* last maximum snd_cwnd */
	u32	last_cwnd;	/* the last snd_cwnd */
	u32	last_time;	/* time when updated last_cwnd */
	u32	bic_origin_point;/* origin point of bic function */
	u32	bic_K;		/* time to origin point
				   from the beginning of the current epoch */
	u32	delay_min;	/* min delay (usec) */
	u32	epoch_start;	/* beginning of an epoch */
	u32	ack_cnt;	/* number of acks */
	u32	tcp_cwnd;	/* estimated tcp cwnd */
	u16	unused;
	u8	sample_cnt;	/* number of samples to decide curr_rtt */
	u8	found;		/* the exit point is found? */
	u32	round_start;	/* beginning of each round */
	u32	end_seq;	/* end_seq of the round */
	u32	last_ack;	/* last time when the ACK spacing is close */
	u32	curr_rtt;	/* the minimum rtt of current round */

#ifdef STARLINK_HANDOVER
	/*
	 * we cannot use hrtimer here because of build failure of
	 * sizeof(struct bictcp) > ICSK_CA_PRIV_SIZE.
	 */
	bool	handover_free_pending;
	struct timer_list handover_timer;
#endif /* STARLINK_HANDOVER */
};

static inline void bictcp_reset(struct bictcp *ca)
{
	memset(ca, 0, offsetof(struct bictcp, unused));
	ca->found = 0;
}

static inline u32 bictcp_clock_us(const struct sock *sk)
{
	return tcp_sk(sk)->tcp_mstamp;
}

static inline void bictcp_hystart_reset(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	ca->round_start = ca->last_ack = bictcp_clock_us(sk);
	ca->end_seq = tp->snd_nxt;
	ca->curr_rtt = ~0U;
	ca->sample_cnt = 0;
}

#ifdef STARLINK_HANDOVER
static void leo_handover_timer_init(struct sock *sk);
static void leo_handover_timer_finish(struct sock *sk);
#endif /* STARLINK_HANDOVER */

static void cubictcp_init(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	bictcp_reset(ca);

	if (hystart)
		bictcp_hystart_reset(sk);

	if (!hystart && initial_ssthresh)
		tcp_sk(sk)->snd_ssthresh = initial_ssthresh;

#ifdef STARLINK_HANDOVER
	leo_handover_timer_init(sk);
#endif /* STARLINK_HANDOVER */
}

static void leo_release(struct sock *sk)
{

#ifdef STARLINK_HANDOVER
	leo_handover_timer_finish(sk);
#endif /* STARLINK_HANDOVER */
}

#ifdef STARLINK_HANDOVER
#define SEC_PER_MIN			60
#define NSEC_PER_MIN			(SEC_PER_MIN * NSEC_PER_SEC)
#define STARLINK_HANDOVER_TIME		(12LLU * NSEC_PER_SEC * HZ)
#define STARLINK_HANDOVER_TIME_JITTER	(10LLU * NSEC_PER_MSEC * HZ)
#define STARLINK_HANDOVER_START						\
	(STARLINK_HANDOVER_TIME - STARLINK_HANDOVER_OFFSET_START * NSEC_PER_MSEC * HZ)
#define STARLINK_HANDOVER_END						\
	(STARLINK_HANDOVER_TIME + STARLINK_HANDOVER_OFFSET_END * NSEC_PER_MSEC * HZ)
#define STARLINK_HANDOVER_INTERVAL	(15LLU * NSEC_PER_SEC * HZ)

#define STARLINK_SYNC_INTERVAL	(1LLU * NSEC_PER_MIN)

static s64 starlink_jiffies_base_compute(void)
{
	struct timespec64 tv;
	s64 sjiffies;

	ktime_get_real_ts64(&tv);

	/*
	 * drop more than a minute in order to avoid wrap.
	 * this may cause a negative value, but it is not
	 * a problem when computing current seconds later.
	 */
	sjiffies = ((tv.tv_sec % SEC_PER_MIN) * NSEC_PER_SEC + tv.tv_nsec) * HZ;
	/*
	 * INITIAL_JIFFIES is unnecessary here because
	 * it will be canceled in starlink_jiffies().
	 */
	sjiffies -= jiffies_64 * NSEC_PER_SEC;

	return sjiffies;
}

static void starlink_jiffies_sync_timer_start(void)
{

	hrtimer_start(&starlink_jiffies_sync_timer,
	    ktime_set(0, STARLINK_SYNC_INTERVAL), HRTIMER_MODE_REL_PINNED_SOFT);
}

static enum hrtimer_restart starlink_jiffies_sync(struct hrtimer *hrt)
{
	s64 njiffies;

	starlink_jiffies_sync_timer_start();

	njiffies = starlink_jiffies_base_compute();

	DP("sync jiffies: old: %lld, new: %lld, diff %lld.%09lld\n",
	    starlink_jiffies_base, njiffies,
	    ((njiffies - starlink_jiffies_base + HZ / 2) / HZ / NSEC_PER_SEC) % SEC_PER_MIN,
	    (((njiffies - starlink_jiffies_base + HZ / 2) / HZ) % NSEC_PER_SEC) *
	    ((njiffies >= starlink_jiffies_base) ? 1LL : -1LL));

	/* do not strictly care the race condition. */
	starlink_jiffies_base = njiffies;

	return HRTIMER_NORESTART;
}

static void starlink_time_init(void)
{

	hrtimer_init(&starlink_jiffies_sync_timer, CLOCK_REALTIME,
	    HRTIMER_MODE_REL_PINNED_SOFT);
	starlink_jiffies_sync_timer.function = starlink_jiffies_sync;
	starlink_jiffies_sync(&starlink_jiffies_sync_timer);
}

static void starlink_time_finish(void)
{

	(void)hrtimer_cancel(&starlink_jiffies_sync_timer);
}

static u64 starlink_jiffies(void)
{
	u64 njiffies;

	njiffies = starlink_jiffies_base + jiffies_64 * NSEC_PER_SEC;
	njiffies %= NSEC_PER_MIN * HZ;
	return njiffies;
}

static u64 starlink_time(void)
{

	return (starlink_jiffies() + HZ / 2) / HZ;
}

/*
 * starlink does scan or handover at the fixed timing,
 * 12s, 27s, 42s, 57s for each minute.
 * XXX: only at 27s, handover occurs???
 */
static bool is_starlink_handover(void)
{
	u64 njiffies;

	njiffies = starlink_jiffies() % STARLINK_HANDOVER_INTERVAL;
	return STARLINK_HANDOVER_START <= njiffies &&
	    njiffies <= STARLINK_HANDOVER_END;
}

static void leo_suspend_transmission(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	/* XXX: should compute cwnd??? */
	ca->last_cwnd = tcp_snd_cwnd(tp);

	/* do not use tcp_snd_cwnd_set(tp, 0) warning this as a bug. */
	tp->snd_cwnd = 0;
}

static void leo_resume_transmission(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	tcp_snd_cwnd_set(tp, max(1, ca->last_cwnd));
}

static void leo_handover_timer_reset(struct sock *sk)
{
#ifdef LEO_HANDOVER_TIMER_ONLY
	struct tcp_sock *tp = tcp_sk(sk);
#endif /* LEO_HANDOVER_TIMER_ONLY */
	struct bictcp *ca = inet_csk_ca(sk);
	u64 njiffies;
	s64 timo;

	njiffies = starlink_jiffies() % STARLINK_HANDOVER_INTERVAL;
#ifdef LEO_HANDOVER_TIMER_ONLY
	if (tp->snd_cwnd == 0)
		timo = STARLINK_HANDOVER_END - njiffies;
	else if (njiffies <= STARLINK_HANDOVER_TIME)
		timo = STARLINK_HANDOVER_START - njiffies;
	else
		timo = STARLINK_HANDOVER_START + STARLINK_HANDOVER_INTERVAL
		    - njiffies;
#else /* LEO_HANDOVER_TIMER_ONLY */
	if (njiffies < STARLINK_HANDOVER_START)
		timo = STARLINK_HANDOVER_START - njiffies;
	else if (njiffies < STARLINK_HANDOVER_END)
		timo = STARLINK_HANDOVER_END - njiffies;
	else
		timo = STARLINK_HANDOVER_START + STARLINK_HANDOVER_INTERVAL
		    - njiffies;
#endif /* ! LEO_HANDOVER_TIMER_ONLY */
	DP("handover: timer reset: timo (ms): %lld, start: %llu, time: %llu, "
	    "end: %llu, int.: %llu, nsec (ms): %llu\n",
	    timo / NSEC_PER_MSEC / HZ, STARLINK_HANDOVER_START / HZ,
	    STARLINK_HANDOVER_TIME / HZ, STARLINK_HANDOVER_END / HZ,
	    STARLINK_HANDOVER_INTERVAL / HZ, njiffies / HZ);
	timo /= NSEC_PER_SEC;
	if (timo <= 0)
		timo = 1;
	sk_reset_timer(sk, &ca->handover_timer, jiffies + timo);
}

static void leo_handover_start(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
#ifdef STARLINK_DEBUG
	struct bictcp *ca = inet_csk_ca(sk);
#endif /* STARLINK_DEBUG */

	if (tcp_snd_cwnd(tp) == 0) {
		DP("handover: start: already started???\n");
		return;
	}

	DP("handover: start: cwnd: %d, last max: %d, last: %d, tcp: %d, inflight: %d\n",
	    tcp_snd_cwnd(tp), ca->last_max_cwnd, ca->last_cwnd, ca->tcp_cwnd,
	    tcp_packets_in_flight(tp));

	leo_suspend_transmission(sk);
}

static void leo_handover_end(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
#ifdef STARLINK_DEBUG
	struct bictcp *ca = inet_csk_ca(sk);
#endif /* STARLINK_DEBUG */

	if (tcp_snd_cwnd(tp) != 0) {
		DP("handover: end: already cwnd recovered???\n");
		return;
	}

	leo_resume_transmission(sk);

	DP("handover: end: recover: cwnd: %d, last max: %d, last: %d, tcp: %d, inflight: %d\n",
	    tcp_snd_cwnd(tp), ca->last_max_cwnd, ca->last_cwnd, ca->tcp_cwnd,
	    tcp_packets_in_flight(tp));

	/* wake up the socket if necessary. */
	/* open code tcp_data_snd_check() in tcp_input.c. */
	/*
	 * XXX: should call tcp_push_pending_frames(),
	 * but symbol is missing...
	 */
	if (sk->sk_socket &&
	    test_bit(SOCK_NOSPACE, &sk->sk_socket->flags)) {
		DP("socket: wake up SOCK_NOSPACE: sndbuf: %u, wmem_queued: %u\n",
		    READ_ONCE(sk->sk_sndbuf),
		    READ_ONCE(sk->sk_wmem_queued));
		/*
		 * we cannot use INDIRECT_CALL_1() here.
		 * INDIRECT_CALL_1(sk->sk_write_space, sk_stream_write_space, sk);
		 */
		(*sk->sk_write_space)(sk);
	}
}

static void leo_handover(struct sock *sk)
{
#ifdef LEO_HANDOVER_TIMER_ONLY
	struct tcp_sock *tp = tcp_sk(sk);

	if (tp->snd_cwnd != 0)
		leo_handover_start(sk);
	else
		leo_handover_end(sk);
#else /* LEO_HANDOVER_TIMER_ONLY  */
	struct tcp_sock *tp = tcp_sk(sk);
	u64 njiffies;

	njiffies = starlink_jiffies() % STARLINK_HANDOVER_INTERVAL;
	if (njiffies + STARLINK_HANDOVER_TIME_JITTER >= STARLINK_HANDOVER_END)
		leo_handover_end(sk);
	else if (njiffies + STARLINK_HANDOVER_TIME_JITTER >= STARLINK_HANDOVER_START)
		leo_handover_start(sk);
	else if (tp->snd_cwnd == 0)
		leo_handover_end(sk);
	else
		/* already handover ended, and resumed. */
		DP("handover: already handover recovered???");
#endif /* ! LEO_HANDOVER_TIMER_ONLY  */
	leo_handover_timer_reset(sk);
}

static void leo_handover_cb(struct timer_list *t)
{
	struct bictcp *ca = from_timer(ca, t, handover_timer);
	uintptr_t off = (uintptr_t)inet_csk_ca(NULL);
	struct sock *sk = (struct sock *)((uintptr_t)ca - off);

	bh_lock_sock(sk);
	if (! sock_owned_by_user(sk))
		leo_handover(sk);
	else if (! ca->handover_free_pending) {
		/* delegate our work to leo_release(). */
		sock_hold(sk);
		ca->handover_free_pending = true;
	}
	bh_unlock_sock(sk);

	/* decrement refernce counter incremented in sk_reset_timer(). */
	sock_put(sk);
}

static void
leo_handover_timer_init(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	timer_setup(&ca->handover_timer, leo_handover_cb, 0);
	if (is_starlink_handover())
		leo_suspend_transmission(sk);
	leo_handover_timer_reset(sk);
	ca->handover_free_pending = false;
}

static void
leo_handover_timer_finish(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	(void)sk_stop_timer(sk, &ca->handover_timer);

	if (ca->handover_free_pending)
		sock_put(sk);
	ca->handover_free_pending = false;
}
#endif /* STARLINK_HANDOVER */

static void cubictcp_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	if (event == CA_EVENT_TX_START) {
		struct bictcp *ca = inet_csk_ca(sk);
		u32 now = tcp_jiffies32;
		s32 delta;

		delta = now - tcp_sk(sk)->lsndtime;

		/* We were application limited (idle) for a while.
		 * Shift epoch_start to keep cwnd growth to cubic curve.
		 */
		if (ca->epoch_start && delta > 0) {
			ca->epoch_start += delta;
			if (after(ca->epoch_start, now))
				ca->epoch_start = now;
		}
		return;
	}
}

/* calculate the cubic root of x using a table lookup followed by one
 * Newton-Raphson iteration.
 * Avg err ~= 0.195%
 */
static u32 cubic_root(u64 a)
{
	u32 x, b, shift;
	/*
	 * cbrt(x) MSB values for x MSB values in [0..63].
	 * Precomputed then refined by hand - Willy Tarreau
	 *
	 * For x in [0..63],
	 *   v = cbrt(x << 18) - 1
	 *   cbrt(x) = (v[x] + 10) >> 6
	 */
	static const u8 v[] = {
		/* 0x00 */    0,   54,   54,   54,  118,  118,  118,  118,
		/* 0x08 */  123,  129,  134,  138,  143,  147,  151,  156,
		/* 0x10 */  157,  161,  164,  168,  170,  173,  176,  179,
		/* 0x18 */  181,  185,  187,  190,  192,  194,  197,  199,
		/* 0x20 */  200,  202,  204,  206,  209,  211,  213,  215,
		/* 0x28 */  217,  219,  221,  222,  224,  225,  227,  229,
		/* 0x30 */  231,  232,  234,  236,  237,  239,  240,  242,
		/* 0x38 */  244,  245,  246,  248,  250,  251,  252,  254,
	};

	b = fls64(a);
	if (b < 7) {
		/* a in [0..63] */
		return ((u32)v[(u32)a] + 35) >> 6;
	}

	b = ((b * 84) >> 8) - 1;
	shift = (a >> (b * 3));

	x = ((u32)(((u32)v[shift] + 10) << b)) >> 6;

	/*
	 * Newton-Raphson iteration
	 *                         2
	 * x    = ( 2 * x  +  a / x  ) / 3
	 *  k+1          k         k
	 */
	x = (2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1)));
	x = ((x * 341) >> 10);
	return x;
}

/*
 * Compute congestion window to use.
 */
static inline void bictcp_update(struct bictcp *ca, u32 cwnd, u32 acked)
{
	u32 delta, bic_target, max_cnt;
	u64 offs, t;

	ca->ack_cnt += acked;	/* count the number of ACKed packets */

	if (ca->last_cwnd == cwnd &&
	    (s32)(tcp_jiffies32 - ca->last_time) <= HZ / 32)
		return;

	/* The CUBIC function can update ca->cnt at most once per jiffy.
	 * On all cwnd reduction events, ca->epoch_start is set to 0,
	 * which will force a recalculation of ca->cnt.
	 */
	if (ca->epoch_start && tcp_jiffies32 == ca->last_time)
		goto tcp_friendliness;

	ca->last_cwnd = cwnd;
	ca->last_time = tcp_jiffies32;

	if (ca->epoch_start == 0) {
		ca->epoch_start = tcp_jiffies32;	/* record beginning */
		ca->ack_cnt = acked;			/* start counting */
		ca->tcp_cwnd = cwnd;			/* syn with cubic */

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
		} else {
			/* Compute new K based on
			 * (wmax-cwnd) * (srtt>>3 / HZ) / c * 2^(3*bictcp_HZ)
			 */
			ca->bic_K = cubic_root(cube_factor
					       * (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
		}
	}

	/* cubic function - calc*/
	/* calculate c * time^3 / rtt,
	 *  while considering overflow in calculation of time^3
	 * (so time^3 is done by using 64 bit)
	 * and without the support of division of 64bit numbers
	 * (so all divisions are done by using 32 bit)
	 *  also NOTE the unit of those veriables
	 *	  time  = (t - K) / 2^bictcp_HZ
	 *	  c = bic_scale >> 10
	 * rtt  = (srtt >> 3) / HZ
	 * !!! The following code does not have overflow problems,
	 * if the cwnd < 1 million packets !!!
	 */

	t = (s32)(tcp_jiffies32 - ca->epoch_start);
	t += usecs_to_jiffies(ca->delay_min);
	/* change the unit from HZ to bictcp_HZ */
	t <<= BICTCP_HZ;
	do_div(t, HZ);

	if (t < ca->bic_K)		/* t - K */
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	/* c/rtt * (t-K)^3 */
	delta = (cube_rtt_scale * offs * offs * offs) >> (10+3*BICTCP_HZ);
	if (t < ca->bic_K)                            /* below origin*/
		bic_target = ca->bic_origin_point - delta;
	else                                          /* above origin*/
		bic_target = ca->bic_origin_point + delta;

	/* cubic function - calc bictcp_cnt*/
	if (bic_target > cwnd) {
		ca->cnt = cwnd / (bic_target - cwnd);
	} else {
		ca->cnt = 100 * cwnd;              /* very small increment*/
	}

	/*
	 * The initial growth of cubic function may be too conservative
	 * when the available bandwidth is still unknown.
	 */
	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;	/* increase cwnd 5% per RTT */

tcp_friendliness:
	/* TCP Friendly */
	if (tcp_friendliness) {
		u32 scale = beta_scale;

		delta = (cwnd * scale) >> 3;
		while (ca->ack_cnt > delta) {		/* update tcp cwnd */
			ca->ack_cnt -= delta;
			ca->tcp_cwnd++;
		}

		if (ca->tcp_cwnd > cwnd) {	/* if bic is slower than tcp */
			delta = ca->tcp_cwnd - cwnd;
			max_cnt = cwnd / delta;
			if (ca->cnt > max_cnt)
				ca->cnt = max_cnt;
		}
	}

	/* The maximum rate of cwnd increase CUBIC allows is 1 packet per
	 * 2 packets ACKed, meaning cwnd grows at 1.5x per RTT.
	 */
	ca->cnt = max(ca->cnt, 2U);
}

static void cubictcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

#ifdef STARLINK_HANDOVER
#ifdef LEO_HANDOVER_TIMER_ONLY
	if (tcp_snd_cwnd(tp) == 0)
		return;
#else /* LEO_HANDOVER_TIMER_ONLY */
	if (is_starlink_handover()) {
		if (tcp_snd_cwnd(tp) != 0) {
			DP("handover: missing transmission suspension???\n");
			leo_handover_start(sk);
		}
		return;
	}
	if (tp->snd_cwnd == 0) {
		DP("handover: unrecovered??? forcely recover cwnd.\n");
		leo_handover_end(sk);
	}
#endif /* ! LEO_HANDOVER_TIMER_ONLY */
#endif /* STALRLINK_HANDOVER */

	if (tcp_in_slow_start(tp)) {
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}
	bictcp_update(ca, tcp_snd_cwnd(tp), acked);
	tcp_cong_avoid_ai(tp, ca->cnt, acked);
}

static u32 cubictcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	ca->epoch_start = 0;	/* end of epoch */

	/* Wmax and fast convergence */
	if (tcp_snd_cwnd(tp) < ca->last_max_cwnd && fast_convergence)
		ca->last_max_cwnd = (tcp_snd_cwnd(tp) * (BICTCP_BETA_SCALE + beta))
			/ (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tcp_snd_cwnd(tp);

	return max((tcp_snd_cwnd(tp) * beta) / BICTCP_BETA_SCALE, 2U);
}

static void cubictcp_state(struct sock *sk, u8 new_state)
{
	if (new_state == TCP_CA_Loss) {
		bictcp_reset(inet_csk_ca(sk));
		bictcp_hystart_reset(sk);
	}
}

/* Account for TSO/GRO delays.
 * Otherwise short RTT flows could get too small ssthresh, since during
 * slow start we begin with small TSO packets and ca->delay_min would
 * not account for long aggregation delay when TSO packets get bigger.
 * Ideally even with a very small RTT we would like to have at least one
 * TSO packet being sent and received by GRO, and another one in qdisc layer.
 * We apply another 100% factor because @rate is doubled at this point.
 * We cap the cushion to 1ms.
 */
static u32 hystart_ack_delay(const struct sock *sk)
{
	unsigned long rate;

	rate = READ_ONCE(sk->sk_pacing_rate);
	if (!rate)
		return 0;
	return min_t(u64, USEC_PER_MSEC,
		     div64_ul((u64)sk->sk_gso_max_size * 4 * USEC_PER_SEC, rate));
}

static void hystart_update(struct sock *sk, u32 delay)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 threshold;

	if (after(tp->snd_una, ca->end_seq))
		bictcp_hystart_reset(sk);

	if (hystart_detect & HYSTART_ACK_TRAIN) {
		u32 now = bictcp_clock_us(sk);

		/* first detection parameter - ack-train detection */
		if ((s32)(now - ca->last_ack) <= hystart_ack_delta_us) {
			ca->last_ack = now;

			threshold = ca->delay_min + hystart_ack_delay(sk);

			/* Hystart ack train triggers if we get ack past
			 * ca->delay_min/2.
			 * Pacing might have delayed packets up to RTT/2
			 * during slow start.
			 */
			if (sk->sk_pacing_status == SK_PACING_NONE)
				threshold >>= 1;

			if ((s32)(now - ca->round_start) > threshold) {
				ca->found = 1;
				pr_debug("hystart_ack_train (%u > %u) delay_min %u (+ ack_delay %u) cwnd %u\n",
					 now - ca->round_start, threshold,
					 ca->delay_min, hystart_ack_delay(sk), tcp_snd_cwnd(tp));
				NET_INC_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTTRAINCWND,
					      tcp_snd_cwnd(tp));
				tp->snd_ssthresh = tcp_snd_cwnd(tp);
			}
		}
	}

	if (hystart_detect & HYSTART_DELAY) {
		/* obtain the minimum delay of more than sampling packets */
		if (ca->curr_rtt > delay)
			ca->curr_rtt = delay;
		if (ca->sample_cnt < HYSTART_MIN_SAMPLES) {
			ca->sample_cnt++;
		} else {
			if (ca->curr_rtt > ca->delay_min +
			    HYSTART_DELAY_THRESH(ca->delay_min >> 3)) {
				ca->found = 1;
				NET_INC_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTDELAYDETECT);
				NET_ADD_STATS(sock_net(sk),
					      LINUX_MIB_TCPHYSTARTDELAYCWND,
					      tcp_snd_cwnd(tp));
				tp->snd_ssthresh = tcp_snd_cwnd(tp);
			}
		}
	}
}

static void cubictcp_acked(struct sock *sk, const struct ack_sample *sample)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 delay;

	/* Some calls are for duplicates without timetamps */
	if (sample->rtt_us < 0)
		return;

	/* Discard delay samples right after fast recovery */
	if (ca->epoch_start && (s32)(tcp_jiffies32 - ca->epoch_start) < HZ)
		return;

	delay = sample->rtt_us;
	if (delay == 0)
		delay = 1;

	/* first time call or link delay decreases */
	if (ca->delay_min == 0 || ca->delay_min > delay)
		ca->delay_min = delay;

	/* hystart triggers when cwnd is larger than some threshold */
	if (!ca->found && tcp_in_slow_start(tp) && hystart &&
	    tcp_snd_cwnd(tp) >= hystart_low_window)
		hystart_update(sk, delay);
}

static struct tcp_congestion_ops cubictcp __read_mostly = {
	.init		= cubictcp_init,
	.release	= leo_release,
	.ssthresh	= cubictcp_recalc_ssthresh,
	.cong_avoid	= cubictcp_cong_avoid,
	.set_state	= cubictcp_state,
	.undo_cwnd	= tcp_reno_undo_cwnd,
	.cwnd_event	= cubictcp_cwnd_event,
	.pkts_acked     = cubictcp_acked,
	.owner		= THIS_MODULE,
	.name		= "leo-cubic",
};

BTF_SET8_START(tcp_cubic_check_kfunc_ids)
#ifdef CONFIG_X86
#ifdef CONFIG_DYNAMIC_FTRACE
BTF_ID_FLAGS(func, cubictcp_init)
BTF_ID_FLAGS(func, leo_release)
BTF_ID_FLAGS(func, cubictcp_recalc_ssthresh)
BTF_ID_FLAGS(func, cubictcp_cong_avoid)
BTF_ID_FLAGS(func, cubictcp_state)
BTF_ID_FLAGS(func, cubictcp_cwnd_event)
BTF_ID_FLAGS(func, cubictcp_acked)
#endif
#endif
BTF_SET8_END(tcp_cubic_check_kfunc_ids)

static const struct btf_kfunc_id_set tcp_cubic_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &tcp_cubic_check_kfunc_ids,
};

static int __init cubictcp_register(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct bictcp) > ICSK_CA_PRIV_SIZE);

#ifdef STARLINK_HANDOVER
	starlink_time_init();
	DP("starlink time: %lld.%09lld\n",
	    starlink_time() / NSEC_PER_SEC, starlink_time() % NSEC_PER_SEC);
#endif /* STARLINK_HANDOVER */

	/* Precompute a bunch of the scaling factors that are used per-packet
	 * based on SRTT of 100ms
	 */

	beta_scale = 8*(BICTCP_BETA_SCALE+beta) / 3
		/ (BICTCP_BETA_SCALE - beta);

	cube_rtt_scale = (bic_scale * 10);	/* 1024*c/rtt */

	/* calculate the "K" for (wmax-cwnd) = c/rtt * K^3
	 *  so K = cubic_root( (wmax-cwnd)*rtt/c )
	 * the unit of K is bictcp_HZ=2^10, not HZ
	 *
	 *  c = bic_scale >> 10
	 *  rtt = 100ms
	 *
	 * the following code has been designed and tested for
	 * cwnd < 1 million packets
	 * RTT < 100 seconds
	 * HZ < 1,000,00  (corresponding to 10 nano-second)
	 */

	/* 1/c * 2^2*bictcp_HZ * srtt */
	cube_factor = 1ull << (10+3*BICTCP_HZ); /* 2^40 */

	/* divide by bic_scale and by constant Srtt (100ms) */
	do_div(cube_factor, bic_scale * 10);

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &tcp_cubic_kfunc_set);
	if (ret < 0)
		return ret;
	return tcp_register_congestion_control(&cubictcp);
}

static void __exit cubictcp_unregister(void)
{

#ifdef STARLINK_HANDOVER
	starlink_time_finish();
#endif /* STARLINK_HANDOVER */
	tcp_unregister_congestion_control(&cubictcp);
}

module_init(cubictcp_register);
module_exit(cubictcp_unregister);

MODULE_AUTHOR("Sangtae Ha, Stephen Hemminger");
MODULE_AUTHOR("Motoyuki OHMORI");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP LEO CUBIC for Starlink");
MODULE_VERSION("2.3");
