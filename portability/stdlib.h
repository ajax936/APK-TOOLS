#pragma once
#include_next <stdlib.h>

#ifdef NEED_QSORT_R
void qsort_r(void *base, size_t nmemb, size_t size,
	int (*compar)(const void *, const void *, void *),
	void *arg);
#endif

#ifdef HAVE_BROKEN_QSORT_R
struct __apk_qsort_r_compat_arg {
	int (*compar)(const void *, const void *, void *);
	void *arg;
};

static inline int __apk_qsort_r_compar_compat(void *arg, const void *a, const void *b)
{
	struct __apk_qsort_r_compat_arg *compat_arg = arg;
	return compat_arg->compar(a, b, compat_arg->arg);
}

static inline void __apk_qsort_r(void *base, size_t nmemb, size_t size,
				 int (*compar)(const void *, const void *, void *),
				 void *arg)
{
	struct __apk_qsort_r_compat_arg compat_arg = {
		.compar = compar,
		.arg = arg,
	};
	qsort_r(base, nmemb, size, &compat_arg, __apk_qsort_r_compar_compat);
}
#define qsort_r(...) __apk_qsort_r(__VA_ARGS__)
#endif
