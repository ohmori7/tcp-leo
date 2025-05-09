/*
 * We could not hold hrtimer here in struct bictcp because of
 * build failure of sizeof(struct bictcp) > ICSK_CA_PRIV_SIZE.
 * In addition, hrtimer may be difficult to handle with socket
 * for locking.
 * In case BBR, sizeof(struct bbr) == ICSK_CA_PRIVE_SIZE,
 * and there is no space available.
 */
struct leo {
	struct timer_list handover_timer;
	void *sock;
	u32 *last_snd_cwnd;
};

bool leo_handover_check(struct sock *, u32);
void leo_init(struct sock *, u32 *);
