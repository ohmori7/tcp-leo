#ifdef LEO_NODEBUG
#define DP(...)
#else /* LEO_NODEBUG */
extern bool leo_debug __read_mostly;
#define DP(fmt, ...)	if (leo_debug) printk("%s: " fmt "\n", __func__, ##__VA_ARGS__)
#endif /* ! LEO_NODEBUG */

struct leo;

bool leo_handover_check(struct leo *, u32 *);
bool leo_handover_check_by_index(struct sock *, u32, u32 *);
u32 leo_index_get(struct leo *);
struct leo *leo_init(struct sock *sk, u32 *);
void leo_finish(struct leo *);
__bpf_kfunc void leo_finish_by_index(struct sock *, u32);
