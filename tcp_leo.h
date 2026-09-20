#ifdef LEO_NODEBUG
#define DP(...)
#else /* LEO_NODEBUG */
extern bool leo_debug __read_mostly;
#define DP(fmt, ...)	if (leo_debug) printk("%s: " fmt, __func__, ##__VA_ARGS__)
#endif /* ! LEO_NODEBUG */

bool leo_handover_check(struct sock *, u32 *);
void leo_init(struct sock *, u32 *);
