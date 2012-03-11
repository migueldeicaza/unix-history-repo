/***********************license start***************
 * Copyright (c) 2003-2010  Cavium Inc. (support@cavium.com). All rights
 * reserved.
 *
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.

 *   * Neither the name of Cavium Inc. nor the names of
 *     its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written
 *     permission.

 * This Software, including technical data, may be subject to U.S. export  control
 * laws, including the U.S. Export Administration Act and its  associated
 * regulations, and may be subject to export or import  regulations in other
 * countries.

 * TO THE MAXIMUM EXTENT PERMITTED BY LAW, THE SOFTWARE IS PROVIDED "AS IS"
 * AND WITH ALL FAULTS AND CAVIUM INC. MAKES NO PROMISES, REPRESENTATIONS OR
 * WARRANTIES, EITHER EXPRESS, IMPLIED, STATUTORY, OR OTHERWISE, WITH RESPECT TO
 * THE SOFTWARE, INCLUDING ITS CONDITION, ITS CONFORMITY TO ANY REPRESENTATION OR
 * DESCRIPTION, OR THE EXISTENCE OF ANY LATENT OR PATENT DEFECTS, AND CAVIUM
 * SPECIFICALLY DISCLAIMS ALL IMPLIED (IF ANY) WARRANTIES OF TITLE,
 * MERCHANTABILITY, NONINFRINGEMENT, FITNESS FOR A PARTICULAR PURPOSE, LACK OF
 * VIRUSES, ACCURACY OR COMPLETENESS, QUIET ENJOYMENT, QUIET POSSESSION OR
 * CORRESPONDENCE TO DESCRIPTION. THE ENTIRE  RISK ARISING OUT OF USE OR
 * PERFORMANCE OF THE SOFTWARE LIES WITH YOU.
 ***********************license end**************************************/







/**
 * @file
 *
 * Fixes and workaround for Octeon chip errata. This file
 * contains functions called by cvmx-helper to workaround known
 * chip errata. For the most part, code doesn't need to call
 * these functions directly.
 *
 * <hr>$Revision: 70030 $<hr>
 */
#ifndef __CVMX_HELPER_ERRATA_H__
#define __CVMX_HELPER_ERRATA_H__

/**
 * @INTERNAL
 * Function to adjust internal IPD pointer alignments
 *
 * @return 0 on success
 *         !0 on failure
 */
extern int __cvmx_helper_errata_fix_ipd_ptr_alignment(void);

/**
 * This function needs to be called on all Octeon chips with
 * errata PKI-100.
 *
 * The Size field is 8 too large in WQE and next pointers
 *
 *  The Size field generated by IPD is 8 larger than it should
 *  be. The Size field is <55:40> of both:
 *      - WORD3 in the work queue entry, and
 *      - the next buffer pointer (which precedes the packet data
 *        in each buffer).
 *
 * @param work   Work queue entry to fix
 * @return Zero on success. Negative on failure
 */
extern int cvmx_helper_fix_ipd_packet_chain(cvmx_wqe_t *work);

/**
 * Due to errata G-720, the 2nd order CDR circuit on CN52XX pass
 * 1 doesn't work properly. The following code disables 2nd order
 * CDR for the specified QLM.
 *
 * @param qlm    QLM to disable 2nd order CDR for.
 */
extern void __cvmx_helper_errata_qlm_disable_2nd_order_cdr(int qlm);
#endif
