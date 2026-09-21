#include <linux/btf.h>
#include <linux/btf_ids.h>
#include <linux/module.h>
#include <net/tcp.h>

#include "tcp_leo.h"

/*
 * We could not have timer in congestion control private region
 * because of limited space, ICSK_CA_PRIV_SIZE.
 * For CUBIC, we could not have hrtimer because:
 * 	sizeof(struct bictcp) > ICSK_CA_PRIV_SIZE.
 * In case BBR, sizeof(struct bbr) == ICSK_CA_PRIVE_SIZE,
 * and there is no space available.
 */
struct leo {
	u32 index;
#define TCP_LEO_HRTIMER
#ifdef TCP_LEO_HRTIMER
	struct hrtimer handover_timer;
#else /* TCP_LEO_HRTIMER */
	struct timer_list handover_timer;
#endif /* ! TCP_LEO_HRTIMER */
	struct sock *sock;
	u32 last_snd_cwnd;
};

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
	(LEO_HANDOVER_START + LEO_HANDOVER_DURATION * NSEC_PER_MSEC * HZ)
#define LEO_HANDOVER_INTERVAL	(15LLU * NSEC_PER_SEC * HZ)

#define LEO_SYNC_INTERVAL		(1LLU * NSEC_PER_MIN)

static s64 leo_jiffies_base __read_mostly;
#define LEO_HANDOVER_OFFSET_DEFAULT	(200ULL)
#define LEO_HANDOVER_DURATION_DEFAULT	(LEO_HANDOVER_OFFSET_DEFAULT << 1)
#define LEO_HANDOVER_OFFSET_MAX		(1000ULL)
#define __LEO_HANDOVER_OFFSET(v)					\
	((u64)(v) <= LEO_HANDOVER_OFFSET_MAX ?				\
	 (u64)(v) : LEO_HANDOVER_OFFSET_MAX)
#define LEO_HANDOVER_OFFSET_START					\
	__LEO_HANDOVER_OFFSET(leo_handover_start_ms)
#define LEO_HANDOVER_DURATION						\
	__LEO_HANDOVER_OFFSET(leo_handover_duration_ms)
static int leo_handover_start_ms __read_mostly =
    LEO_HANDOVER_OFFSET_DEFAULT;
static int leo_handover_duration_ms __read_mostly =
    LEO_HANDOVER_DURATION_DEFAULT;
static struct hrtimer leo_jiffies_sync_timer;

module_param(leo_debug, bool, 0644);
MODULE_PARM_DESC(leo_debug, "debug flag");
module_param(leo_handover_start_ms, uint, 0644);
MODULE_PARM_DESC(leo_handover_start_ms, "starting offset of handover (0<=offset<=1000)");
module_param(leo_handover_duration_ms, uint, 0644);
MODULE_PARM_DESC(leo_handover_duration_ms, "duration of handover (0<=duration<=1000)");

#define LEO_SIZE_DEFAULT 128
#define LEO_INDEX_INIT	1
static u32 leo_index = 0;
static u32 leo_size = 0;
static struct leo **leos = NULL;
static DEFINE_MUTEX(leo_lock);

void
leo_printk(const char *fmt, ...)
{
	struct timespec64 ts;
	va_list args;
#define LEO_DEBUG_BUFSIZE	256
	char buf[LEO_DEBUG_BUFSIZE];
	int len;

	ktime_get_real_ts64(&ts);
	len = snprintf(buf, sizeof(buf), "[%02lld.%09ld] ",
	    ts.tv_sec % 60, ts.tv_nsec);
	if (len <= 0)
		return;
	va_start(args, fmt);
	len = vsnprintf(buf + len, sizeof(buf) - len, fmt, args);
	buf[sizeof(buf) - 1] = '\0'; /* just in case. */
	va_end(args);
	if (len > 0)
		printk(buf);
}
EXPORT_SYMBOL(leo_printk);

static u32
leo_index_next(void)
{
	u32 i;

	WARN_ON(leo_index == LEO_INDEX_NONE);
	WARN_ON(leo_index > leo_size);
	if (leo_index == LEO_INDEX_NONE ||
	    leo_index > leo_size)
		return LEO_INDEX_NONE;
	for (i = leo_index; i <= leo_size; i++)
		if (leos[i - 1] == NULL)
			return i;
	for (i = LEO_INDEX_INIT; i < leo_index; i++)
		if (leos[i - 1] == NULL)
			return i;
	return LEO_INDEX_NONE;
}

static bool
leo_index_alloc(struct leo *leo)
{

	mutex_lock(&leo_lock);
	leo->index = leo_index_next();
	if (leo->index != LEO_INDEX_NONE) {
		leos[leo->index - 1] = leo;
		leo_index = leo->index;
	}
	mutex_unlock(&leo_lock);
	return (leo->index != LEO_INDEX_NONE);
}

u32
leo_index_get(struct leo *leo)
{

	if (leo == NULL)
		return LEO_INDEX_NONE;
	return leo->index;
}
EXPORT_SYMBOL(leo_index_get);

static void
leo_index_free(struct leo *leo)
{

	mutex_lock(&leo_lock);
	WARN_ON(leo->index == LEO_INDEX_NONE);
	WARN_ON(leo->index > leo_size);
	WARN_ON(leos[leo->index - 1] != leo);
	if (leo->index != LEO_INDEX_NONE &&
	    leo->index <= leo_size &&
	    leos[leo->index - 1] == leo)
		leos[leo->index - 1] = NULL;
	leo->index = LEO_INDEX_NONE;
	mutex_unlock(&leo_lock);
}

static struct leo *
leo_lookup(struct sock *sk, u32 idx)
{
	struct leo *leo;

	if (idx == LEO_INDEX_NONE)
		return NULL;
	WARN_ON(idx > leo_size);
	if (idx > leo_size)
		return NULL;
	leo = leos[idx - 1];
	WARN_ON(leo == NULL);
	WARN_ON(leo->sock != sk);
	if (leo == NULL || leo->sock != sk)
		return NULL;
	return leo;
}


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

	DP("LEO: sync jiffies: old: %lld, new: %lld, diff %lld.%09lld",
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
	return leo_handover_start_ms + leo_handover_duration_ms;
}

__bpf_kfunc static void
leo_suspend_transmission(struct leo *leo)
{
	struct sock *sk = leo->sock;
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);

	leo->last_snd_cwnd = tcp_snd_cwnd(tp);

	/* prevent wrong computation in tcp_input.c: tcp_cwnd_reduction(). */
	if (tp->prior_cwnd == 0)
		tp->prior_cwnd = tp->snd_cwnd;

	/* do not use tcp_snd_cwnd_set(tp, 0) warning this as a bug. */
	tp->snd_cwnd = 0;

	/* extend retransmission and other timeouts. */
	/* XXX: consider timer granularity for more accuracy. */
	if (icsk->icsk_timeout != 0)
		icsk->icsk_timeout += msecs_to_jiffies(leo_handover_duration(sk));
}

__bpf_kfunc static void
leo_resume_transmission(struct leo *leo)
{
	struct sock *sk = leo->sock;
	struct tcp_sock *tp = tcp_sk(sk);

	WARN_ON(leo->last_snd_cwnd == 0);
	tcp_snd_cwnd_set(tp, max(1, leo->last_snd_cwnd));

	/* wake up the socket if necessary. */
	/* open code of tcp_data_snd_check() in tcp_input.c. */
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
		DP("LEO[%u:%p]: wake up SOCK_NOSPACE: sndbuf: %u, wmem_queued: %u",
		    leo->index, sk, READ_ONCE(sk->sk_sndbuf), READ_ONCE(sk->sk_wmem_queued));
		/*
		 * we cannot use INDIRECT_CALL_1() here.
		 * INDIRECT_CALL_1(sk->sk_write_space, sk_stream_write_space, sk);
		 */
		(*sk->sk_write_space)(sk);
	}
#endif /* ! TCP_LEO */
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
	if (njiffies + LEO_HANDOVER_TIME_JITTER < LEO_HANDOVER_START)
		timo = LEO_HANDOVER_START - njiffies;
	else if (njiffies + LEO_HANDOVER_TIME_JITTER < LEO_HANDOVER_END)
		timo = LEO_HANDOVER_END - njiffies;
	else
		timo = LEO_HANDOVER_START + LEO_HANDOVER_INTERVAL
		    - njiffies;
#endif /* ! LEO_HANDOVER_TIMER_ONLY */
	DP("LEO[%u:%p]: handover: timer reset: timo (ms): %lld, start: %llu, time: %llu, "
	    "end: %llu, int.: %llu, nsec (ms): %llu",
	    leo->index, sk, timo / NSEC_PER_MSEC / HZ, LEO_HANDOVER_START / HZ,
	    LEO_HANDOVER_TIME / HZ, LEO_HANDOVER_END / HZ,
	    LEO_HANDOVER_INTERVAL / HZ, njiffies / HZ);
#ifdef TCP_LEO_HRTIMER
	timo /= HZ;
	if (timo <= 0)
		timo = 1;
	sock_hold(sk);
	hrtimer_start(&leo->handover_timer,
	    ktime_set(timo / NSEC_PER_SEC, timo % NSEC_PER_SEC),
	    HRTIMER_MODE_REL_PINNED_SOFT);
#else /* TCP_LEO_HRTIMER */
	timo /= NSEC_PER_SEC;
	if (timo <= 0)
		timo = 1;
	/* refernce counter will be incremented in sk_reset_timer(). */
	sk_reset_timer(sk, &leo->handover_timer, jiffies + timo);
#endif /* ! TCP_LEO_HRTIMER */
}

static void
leo_handover_start(struct leo *leo)
{
	struct sock *sk = leo->sock;
	struct tcp_sock *tp = tcp_sk(sk);

	if (tcp_snd_cwnd(tp) == 0) {
		DP("LEO[%u:%p]: handover: start: already started???",
		    leo->index, sk);
		return;
	}

	DP("LEO[%u:%p]: handover: start: cwnd: %d, inflight: %d",
	    leo->index, sk, tcp_snd_cwnd(tp), tcp_packets_in_flight(tp));

	leo_suspend_transmission(leo);
}

static void
leo_handover_end(struct leo *leo)
{
	struct sock *sk = leo->sock;
	struct tcp_sock *tp = tcp_sk(sk);

	if (tcp_snd_cwnd(tp) != 0) {
		DP("LEO[%u:%p]: handover: end: already cwnd recovered???",
		    leo->index, sk);
		return;
	}

	leo_resume_transmission(leo);

	DP("LEO[%u:%p]: handover: end: recover: cwnd: %d, inflight: %d",
	    leo->index, sk, tcp_snd_cwnd(tp), tcp_packets_in_flight(tp));
}

bool
leo_handover_check(struct leo *leo)
{
	struct sock *sk;
	struct tcp_sock *tp;

	WARN_ON(leo == NULL);
	if (leo == NULL) /* just in case. */
		return false;
	sk = leo->sock;
	tp = tcp_sk(sk);
#ifdef LEO_HANDOVER_TIMER_ONLY
	if (tcp_snd_cwnd(tp) == 0)
		return true;
#else /* LEO_HANDOVER_TIMER_ONLY */
	if (is_leo_handover()) {
		if (tcp_snd_cwnd(tp) != 0) {
			DP("LEO[%u:%p]: handover: missing transmission suspension???",
			    leo->index, sk);
			leo_handover_start(leo);
		}
		return true;
	}
	if (tcp_snd_cwnd(tp) == 0) {
		DP("LEO[%u:%p]: handover: unrecovered??? forcely recover cwnd.",
		    leo->index, sk);
		leo_handover_end(leo);
	}
#endif /* ! LEO_HANDOVER_TIMER_ONLY */
	return false;
}
EXPORT_SYMBOL(leo_handover_check);

bool
leo_handover_check_by_index(struct sock *sk, u32 idx)
{
	struct leo *leo;

	WARN_ON(idx > leo_size);
	if (idx > leo_size)
		return false;
	leo = leos[idx - 1];
	if (leo == NULL || leo->sock != sk)
		return false;
	return leo_handover_check(leo);
}
EXPORT_SYMBOL(leo_handover_check_by_index);

static void
leo_handover(struct leo *leo)
{
	struct sock *sk = LEO_SOCKET(leo);
	struct tcp_sock *tp = tcp_sk(sk);
#ifdef LEO_HANDOVER_TIMER_ONLY

	if (tp->snd_cwnd != 0)
		leo_handover_start(leo);
	else
		leo_handover_end(leo);
#else /* LEO_HANDOVER_TIMER_ONLY  */
	u64 njiffies;

	njiffies = leo_jiffies() % LEO_HANDOVER_INTERVAL;
	if (njiffies + LEO_HANDOVER_TIME_JITTER >= LEO_HANDOVER_END)
		leo_handover_end(leo);
	else if (njiffies + LEO_HANDOVER_TIME_JITTER >= LEO_HANDOVER_START)
		leo_handover_start(leo);
	else if (tp->snd_cwnd == 0)
		leo_handover_end(leo);
	else
		/* already handover ended, and resumed. */
		DP("LEO[%u:%p]: handover: already handover recovered???",
		    leo->index, sk);
#endif /* ! LEO_HANDOVER_TIMER_ONLY  */
	leo_handover_timer_reset(leo);
}

#ifdef TCP_LEO_HRTIMER
__bpf_kfunc static enum hrtimer_restart
leo_handover_timeout(struct hrtimer *t)
#else /* TCP_LEO_HRTIMER */
__bpf_kfunc static void
leo_handover_timeout(struct timer_list *t)
#endif /* ! TCP_LEO_HRTIMER */
{
#ifdef TCP_LEO_HRTIMER
	struct leo *leo = container_of(t, struct leo, handover_timer);
#else /* TCP_LEO_HRTIMER */
	struct leo *leo = from_timer(leo, t, handover_timer);
#endif /* ! TCP_LEO_HRTIMER */
	struct sock *sk = LEO_SOCKET(leo);

	bh_lock_sock(sk);
	if (sock_owned_by_user(sk)) {
#ifdef TCP_LEO_HRTIMER
		hrtimer_start(&leo->handover_timer,
		    ktime_set(0, NSEC_PER_MSEC),
		    HRTIMER_MODE_REL_PINNED_SOFT);
		sock_hold(sk);
#else /* TCP_LEO_HRTIMER */
		sk_reset_timer(sk, &leo->handover_timer, jiffies + 1);
#endif /* ! TCP_LEO_HRTIMER */
		DP("LEO[%u:%p]: socket is owned by user", leo->index, sk);
	} else if (sk->sk_state == TCP_ESTABLISHED)
		leo_handover(leo);
	bh_unlock_sock(sk);

#if ! defined(TCP_LEO_HRTIMER)
	/* decrement refernce counter incremented in sk_reset_timer(). */
#endif /* ! TCP_LEO_HRTIMER */
	sock_put(sk);

#ifdef TCP_LEO_HRTIMER
	return HRTIMER_NORESTART;
#endif /* TCP_LEO_HRTIMER */
}

__bpf_kfunc struct leo *
leo_init(struct sock *sk)
{
	struct leo *leo;

	leo = kmalloc(sizeof(*leo), GFP_ATOMIC);
	if (leo == NULL) {
		DP("LEO[?:%p]: allocation failure", sk);
		return NULL;
	}
	if (! leo_index_alloc(leo)) {
		DP("LEO[?:%p]: index overflow", sk);
		kfree(leo);
		return NULL;
	}
	DP("LEO[%u:%p]: allocate: %p", leo->index, sk, leo);

	leo->sock = sk;
	leo->last_snd_cwnd = 0;

#ifdef TCP_LEO_HRTIMER
	hrtimer_init(&leo->handover_timer, CLOCK_REALTIME,
	    HRTIMER_MODE_REL_PINNED_SOFT);
	leo->handover_timer.function = leo_handover_timeout;
#else /* TCP_LEO_HRTIMER */
	timer_setup(&leo->handover_timer, leo_handover_timeout, 0);
#endif /* TCP_LEO_HRTIMER */
	if (is_leo_handover())
		leo_suspend_transmission(leo);
	leo_handover_timer_reset(leo);
	return leo;
}
EXPORT_SYMBOL(leo_init);

__bpf_kfunc void
leo_finish(struct leo *leo)
{

	WARN_ON(leo == NULL);
	WARN_ON(leo->index == LEO_INDEX_NONE);
	WARN_ON(leo->index > leo_size);
	if (leo == NULL ||
	    leo->index == LEO_INDEX_NONE ||
	    leo->index > leo_size)
		return;

#ifdef TCP_LEO_HRTIMER
	if (hrtimer_try_to_cancel(&leo->handover_timer) == 1)
                sock_put(leo->sock);
#endif /* TCP_LEO_HRTIMER */

	DP("LEO[%u:%p]: free: %p", leo->index, LEO_SOCKET(leo), leo);
	leo_index_free(leo);
	kfree(leo);
}
EXPORT_SYMBOL(leo_finish);

__bpf_kfunc void
leo_finish_by_index(struct sock *sk, u32 idx)
{

	leo_finish(leo_lookup(sk, idx));
}
EXPORT_SYMBOL(leo_finish_by_index);

BTF_SET8_START(leo_check_kfunc_ids)
#ifdef CONFIG_X86
#ifdef CONFIG_DYNAMIC_FTRACE
BTF_ID_FLAGS(func, leo_suspend_transmission)
BTF_ID_FLAGS(func, leo_resume_transmission)
BTF_ID_FLAGS(func, leo_handover_timeout)
BTF_ID_FLAGS(func, leo_init)
BTF_ID_FLAGS(func, leo_finish)
BTF_ID_FLAGS(func, leo_finish_by_index)
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

	leos = kmalloc(sizeof(struct leo *) * LEO_SIZE_DEFAULT, GFP_ATOMIC);
	if (leos == NULL)
		return -1;
	memset(leos, 0, sizeof(struct leo *) * LEO_SIZE_DEFAULT);

	ret = register_btf_kfunc_id_set(BPF_PROG_TYPE_STRUCT_OPS, &leo_kfunc_set);
	if (ret < 0) {
		kfree(leos);
		return ret;
	}

	leo_size = LEO_SIZE_DEFAULT;
	leo_index = LEO_INDEX_INIT;
	leo_time_init();
	DP("LEO: time: %lld.%09lld",
	    leo_time() / NSEC_PER_SEC, leo_time() % NSEC_PER_SEC);

	return 0;
}

static void __exit
leo_unregister(void)
{

	kfree(leos);
	leos = NULL;
	leo_size = 0;
	leo_index = LEO_INDEX_NONE;
	leo_time_finish();
}

module_init(leo_register);
module_exit(leo_unregister);
 
MODULE_AUTHOR("Motoyuki OHMORI");
/* XXX: i would like to make this BSD license but hrtimer() is under GPL... */
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("TCP LEO for Starlink");
MODULE_VERSION("0.2");
