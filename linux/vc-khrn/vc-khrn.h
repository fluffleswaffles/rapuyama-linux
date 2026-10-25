// SPDX-License-Identifier: GPL-2.0-only
/*
 * /dev/vc-khrn, the khronos rpc channel of drivers/soc/rapuyama/videocore.c.
 * same layout as struct vc_khrn_msg there
 */
#ifndef VC_KHRN_H
#define VC_KHRN_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VC_FOURCC(a, b, c, d) ((__u32)(a) | (__u32)(b) << 8 | (__u32)(c) << 16 | (__u32)(d) << 24)
#define VC_FCC_KHRN VC_FOURCC('K', 'H', 'R', 'N')
#define VC_FCC_UPDH VC_FOURCC('U', 'P', 'D', 'H')
#define VC_FCC_DISP VC_FOURCC('D', 'I', 'S', 'P')

struct vc_khrn_msg{
	__u32 fourcc;		/* VC_FCC_KHRN or VC_FCC_DISP */
	__u32 len;		/* message bytes */
	__u64 data;
	__u64 bulk;		/* the pieces back to back */
	__u64 bulk_sizes;	/* __u32 per piece, each one VCHI bulk */
	__u32 nbulk;
	__u32 reply_max;	/* 0 = no reply, else wait for one message */
	__u64 reply;
	__u32 reply_len;	/* out */
	__u32 pad;
};

#define VC_KHRN_IOC_MSG _IOWR('V', 1, struct vc_khrn_msg)
#define VC_KHRN_IOC_DISPLAY _IOR('V', 2, __u32)
#define VC_KHRN_IOC_FEATURES _IOR('V', 3, __u32)
#define VC_KHRN_FEATURE_UPDH 1

#endif