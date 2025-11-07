#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/module.h>
#include <net/tcp.h>

#include "tcp_leo.h"

bool leo_debug __read_mostly = false;
EXPORT_SYMBOL(leo_debug);

#define LEO_SOCKET(leo)	((leo)->sock)

#define SEC_PER_MIN			60
#define NSEC_PER_MIN			(SEC_PER_MIN * NSEC_PER_SEC)
#define LEO_HANDOVER_TIME		(12LLU * NSEC_PER_SEC * HZ)
#define LEO_HANDOVER_TIME_JITTER	(10LLU * NSEC_PER_MSEC * HZ)
#define LEO_HANDOVER_START						\
	(LEO_HANDOVER_TIME - LEO_HANDOVER_OFFSET_START * NSEC_PER_MSEC * HZ)
#define LEO_HANDOVER_END						\
	(LEO_HANDOVER_TIME + LEO_HANDOVER_OFFSET_END * NSEC_PER_MSEC * HZ)
#define LEO_HANDOVER_INTERVAL	(15LLU * NSEC_PER_SEC * HZ)

#define LEO_SYNC_INTERVAL		(1LLU * NSEC_PER_MIN)

static s64 leo_jiffies_base __read_mostly;
#define LEO_HANDOVER_OFFSET_DEFAULT	(200ULL)
#define LEO_HANDOVER_OFFSET_MAX		(1000ULL)
#define __LEO_HANDOVER_OFFSET(v)					\
	((u64)(v) <= LEO_HANDOVER_OFFSET_MAX ?				\
	 (u64)(v) : LEO_HANDOVER_OFFSET_MAX)
#define LEO_HANDOVER_OFFSET_START					\
	__LEO_HANDOVER_OFFSET(leo_handover_start_ms)
#define LEO_HANDOVER_OFFSET_END						\
	__LEO_HANDOVER_OFFSET(leo_handover_end_ms)
static int leo_handover_start_ms __read_mostly =
    LEO_HANDOVER_OFFSET_DEFAULT;
static int leo_handover_end_ms __read_mostly =
    LEO_HANDOVER_OFFSET_DEFAULT;
static struct hrtimer leo_jiffies_sync_timer;

/* XXX */
static void leo_finish(struct leo *);

module_param(leo_debug, bool, 0644);
MODULE_PARM_DESC(leo_debug, "debug flag");
module_param(leo_handover_start_ms, int, 0644);
MODULE_PARM_DESC(leo_handover_start_ms, "starting offset of handover (0<=offset<=1000)");
module_param(leo_handover_end_ms, int, 0644);
MODULE_PARM_DESC(leo_handover_end_ms, "ending offset of handover (0<=offset<=1000)");

static s64
leo_jiffies_base_compute(void)
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
	 * it will be canceled in leo_jiffies().
	 */
	sjiffies -= jiffies_64 * NSEC_PER_SEC;

	return sjiffies;
}

static void
leo_jiffies_sync_timer_start(void)
{

	hrtimer_start(&leo_jiffies_sync_timer,
	    ktime_set(0, LEO_SYNC_INTERVAL), HRTIMER_MODE_REL_PINNED_SOFT);
}

static enum hrtimer_restart
leo_jiffies_sync(struct hrtimer *hrt)
{
	s64 njiffies;

	leo_jiffies_sync_timer_start();

	njiffies = leo_jiffies_base_compute();

	DP("LEO: sync jiffies: old: %lld, new: %lld, diff %lld.%09lld\n",
	    leo_jiffies_base, njiffies,
	    ((njiffies - leo_jiffies_base + HZ / 2) / HZ / NSEC_PER_SEC) % SEC_PER_MIN,
	    (((njiffies - leo_jiffies_base + HZ / 2) / HZ) % NSEC_PER_SEC) *
	    ((njiffies >= leo_jiffies_base) ? 1LL : -1LL));

	/* do not strictly care the race condition. */
	leo_jiffies_base = njiffies;

	return HRTIMER_NORESTART;
}

static void
leo_time_init(void)
{

	hrtimer_init(&leo_jiffies_sync_timer, CLOCK_REALTIME,
	    HRTIMER_MODE_REL_PINNED_SOFT);
	leo_jiffies_sync_timer.function = leo_jiffies_sync;
	leo_jiffies_sync(&leo_jiffies_sync_timer);
}

static void
leo_time_finish(void)
{

	(void)hrtimer_cancel(&leo_jiffies_sync_timer);
}

static u64
leo_jiffies(void)
{
	u64 njiffies;

	njiffies = leo_jiffies_base + jiffies_64 * NSEC_PER_SEC;
	njiffies %= NSEC_PER_MIN * HZ;
	return njiffies;
}

#if ! defined(LEO_NODEBUG)
static u64
leo_time(void)
{

	return (leo_jiffies() + HZ / 2) / HZ;
}
#endif /* ! LEO_NODEBUG */

/*
 * leo does scan or handover at the fixed timing,
 * 12s, 27s, 42s, 57s for each minute.
 * XXX: only at 27s, handover occurs???
 */
static bool
is_leo_handover(void)
{
	u64 njiffies;

	njiffies = leo_jiffies() % LEO_HANDOVER_INTERVAL;
	return LEO_HANDOVER_START <= njiffies &&
	    njiffies <= LEO_HANDOVER_END;
}

static unsigned long
leo_handover_duration(struct sock *sk)
{

	(void)sk; /* XXX: different duration per socket in future. */
	return leo_handover_start_ms + leo_handover_end_ms;
}

__bpf_kfunc static void
leo_suspend_transmission(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);

#if 0	/* XXX: no space to hold last_snd_cwnd... */
	/* XXX: should compute cwnd??? */
	leo->last_snd_cwnd = tcp_snd_cwnd(tp);
#endif /* 0 */

	/* do not use tcp_snd_cwnd_set(tp, 0) warning this as a bug. */
	tp->snd_cwnd = 0;

	/* extend retransmission and other timeouts. */
	/* XXX: consider timer granularity for more accuracy. */
	if (icsk->icsk_timeout != 0)
		icsk->icsk_timeout += msecs_to_jiffies(leo_handover_duration(sk));
}

__bpf_kfunc static void
leo_resume_transmission(struct sock *sk, u32 last_snd_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tcp_snd_cwnd_set(tp, max(1, last_snd_cwnd));
}

static void
leo_handover_timer_reset(struct leo *leo)
{
	struct sock *sk = LEO_SOCKET(leo);
#ifdef LEO_HANDOVER_TIMER_ONLY
	struct tcp_sock *tp = tcp_sk(sk);
#endif /* LEO_HANDOVER_TIMER_ONLY */
	u64 njiffies;
	s64 timo;

	njiffies = leo_jiffies() % LEO_HANDOVER_INTERVAL;
#ifdef LEO_HANDOVER_TIMER_ONLY
	if (tp->snd_cwnd == 0)
		timo = LEO_HANDOVER_END - njiffies;
	else if (njiffies <= LEO_HANDOVER_TIME)
		timo = LEO_HANDOVER_START - njiffies;
	else
		timo = LEO_HANDOVER_START + LEO_HANDOVER_INTERVAL
		    - njiffies;
#else /* LEO_HANDOVER_TIMER_ONLY */
	if (njiffies < LEO_HANDOVER_START)
		timo = LEO_HANDOVER_START - njiffies;
	else if (njiffies < LEO_HANDOVER_END)
		timo = LEO_HANDOVER_END - njiffies;
	else
		timo = LEO_HANDOVER_START + LEO_HANDOVER_INTERVAL
		    - njiffies;
#endif /* ! LEO_HANDOVER_TIMER_ONLY */
	DP("LEO[%p]: handover: timer reset: timo (ms): %lld, start: %llu, time: %llu, "
	    "end: %llu, int.: %llu, nsec (ms): %llu\n",
	    sk, timo / NSEC_PER_MSEC / HZ, LEO_HANDOVER_START / HZ,
	    LEO_HANDOVER_TIME / HZ, LEO_HANDOVER_END / HZ,
	    LEO_HANDOVER_INTERVAL / HZ, njiffies / HZ);
	timo /= NSEC_PER_SEC;
	if (timo <= 0)
		timo = 1;
	sk_reset_timer(sk, &leo->handover_timer, jiffies + timo);
}

static void
leo_handover_start(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (tcp_snd_cwnd(tp) == 0) {
		DP("LEO[%p]: handover: start: already started???\n", sk);
		return;
	}

	DP("LEO[%p]: handover: start: cwnd: %d, inflight: %d\n",
	    sk, tcp_snd_cwnd(tp), tcp_packets_in_flight(tp));

	leo_suspend_transmission(sk);
}

static void
leo_handover_end(struct sock *sk, u32 last_snd_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (tcp_snd_cwnd(tp) != 0) {
		DP("LEO[%p]: handover: end: already cwnd recovered???\n", sk);
		return;
	}

	leo_resume_transmission(sk, last_snd_cwnd);

	DP("LEO[%p]: handover: end: recover: cwnd: %d, inflight: %d\n",
	    sk, tcp_snd_cwnd(tp), tcp_packets_in_flight(tp));

	/* wake up the socket if necessary. */
	/* open code tcp_data_snd_check() in tcp_input.c. */
#ifdef TCP_LEO
	/*
	 * XXX: these functions are not exported by default, and
	 *	thus require kernel modifications.
	 */
	tcp_push_pending_frames(sk);
	tcp_check_space(sk);
#else /* TCP_LEO */
	/*
	 * XXX: should call tcp_push_pending_frames(),
	 * but symbol is missing...
	 */
	if (sk->sk_socket &&
	    test_bit(SOCK_NOSPACE, &sk->sk_socket->flags)) {
		DP("LEO[%p]: wake up SOCK_NOSPACE: sndbuf: %u, wmem_queued: %u\n",
		    sk, READ_ONCE(sk->sk_sndbuf), READ_ONCE(sk->sk_wmem_queued));
		/*
		 * we cannot use INDIRECT_CALL_1() here.
		 * INDIRECT_CALL_1(sk->sk_write_space, sk_stream_write_space, sk);
		 */
		(*sk->sk_write_space)(sk);
	}
#endif /* ! TCP_LEO */
}

bool
leo_handover_check(struct sock *sk, u32 last_snd_cwnd)
{
	struct tcp_sock *tp = tcp_sk(sk);

#ifdef LEO_HANDOVER_TIMER_ONLY
	if (tcp_snd_cwnd(tp) == 0)
		return true;
#else /* LEO_HANDOVER_TIMER_ONLY */
	if (is_leo_handover()) {
		if (tcp_snd_cwnd(tp) != 0) {
			DP("LEO[%p]: handover: missing transmission suspension???\n", sk);
			leo_handover_start(sk);
		}
		return true;
	}
	if (tcp_snd_cwnd(tp) == 0) {
		DP("LEO[%p]: handover: unrecovered??? forcely recover cwnd.\n", sk);
		leo_handover_end(sk, last_snd_cwnd);
	}
#endif /* ! LEO_HANDOVER_TIMER_ONLY */
	return false;
}
EXPORT_SYMBOL(leo_handover_check);

static void
leo_handover(struct leo *leo)
{
	struct sock *sk = LEO_SOCKET(leo);
	struct tcp_sock *tp = tcp_sk(sk);
#ifdef LEO_HANDOVER_TIMER_ONLY

	if (tp->snd_cwnd != 0)
		leo_handover_start(sk);
	else
		leo_handover_end(sk, *leo->last_snd_cwnd);
#else /* LEO_HANDOVER_TIMER_ONLY  */
	u64 njiffies;

	njiffies = leo_jiffies() % LEO_HANDOVER_INTERVAL;
	if (njiffies + LEO_HANDOVER_TIME_JITTER >= LEO_HANDOVER_END)
		leo_handover_end(sk, *leo->last_snd_cwnd);
	else if (njiffies + LEO_HANDOVER_TIME_JITTER >= LEO_HANDOVER_START)
		leo_handover_start(sk);
	else if (tp->snd_cwnd == 0)
		leo_handover_end(sk, *leo->last_snd_cwnd);
	else
		/* already handover ended, and resumed. */
		DP("LEO[%p]: handover: already handover recovered???", sk);
#endif /* ! LEO_HANDOVER_TIMER_ONLY  */
	leo_handover_timer_reset(leo);
}

__bpf_kfunc static void
leo_handover_cb(struct timer_list *t)
{
	struct leo *leo = from_timer(leo, t, handover_timer);
	struct sock *sk = LEO_SOCKET(leo);

	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
		sk_reset_timer(sk, &leo->handover_timer, jiffies + 1);
		DP("LEO[%p]: socket is owned by user\n", sk);
	} else if (sk->sk_state != TCP_ESTABLISHED)
		leo_finish(leo);
	else
		leo_handover(leo);
	bh_unlock_sock(sk);

	/* decrement refernce counter incremented in sk_reset_timer(). */
	sock_put(sk);
}

__bpf_kfunc void
leo_init(struct sock *sk, u32 *last_snd_cwnd)
{
	struct leo *leo;

	leo = kmalloc(sizeof(*leo), GFP_ATOMIC);
	if (leo == NULL) {
		DP("LEO[%p]: allocation failure\n", sk);
		return;
	}
	DP("LEO[%p]: allocate: %p\n", sk, leo);

	leo->sock = sk;
	leo->last_snd_cwnd = last_snd_cwnd;

	timer_setup(&leo->handover_timer, leo_handover_cb, 0);
	if (is_leo_handover())
		leo_suspend_transmission(sk);
	leo_handover_timer_reset(leo);
}
EXPORT_SYMBOL(leo_init);

__bpf_kfunc static void
leo_finish(struct leo *leo)
{

	DP("LEO[%p]: free: %p\n", LEO_SOCKET(leo), leo);
	kfree(leo);
}

BTF_SET8_START(leo_check_kfunc_ids)
#ifdef CONFIG_X86
#ifdef CONFIG_DYNAMIC_FTRACE
BTF_ID_FLAGS(func, leo_suspend_transmission)
BTF_ID_FLAGS(func, leo_resume_transmission)
BTF_ID_FLAGS(func, leo_handover_cb)
BTF_ID_FLAGS(func, leo_init)
BTF_ID_FLAGS(func, leo_finish)
#endif
#endif
BTF_SET8_END(leo_check_kfunc_ids)

static const struct btf_kfunc_id_set leo_kfunc_set = {
	.owner = THIS_MODULE,
	.set   = &leo_check_kfunc_ids,
};

static int __init
leo_register(void)
{
	int ret;

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &leo_kfunc_set);
	if (ret < 0)
		return ret;

	leo_time_init();
	DP("LEO: time: %lld.%09lld\n",
	    leo_time() / NSEC_PER_SEC, leo_time() % NSEC_PER_SEC);

	return 0;
}

static void __exit
leo_unregister(void)
{

	leo_time_finish();
}

module_init(leo_register);
module_exit(leo_unregister);
 
MODULE_AUTHOR("Motoyuki OHMORI");
/* XXX: i would like to make this BSD license but hrtimer() is under GPL... */
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TCP LEO for Starlink");
MODULE_VERSION("0.1");
