/*
 * BCMSDH Function Driver for the native SDIO/MMC driver in the Linux Kernel
 *
 * Copyright (C) 1999-2016, Broadcom Corporation
 * 
 *      Unless you and Broadcom execute a separate written software license
 * agreement governing use of this software, this software is licensed to you
 * under the terms of the GNU General Public License version 2 (the "GPL"),
 * available at http://www.broadcom.com/licenses/GPLv2.php, with the
 * following added to such license:
 * 
 *      As a special exception, the copyright holders of this software give you
 * permission to link this software with independent modules, and to copy and
 * distribute the resulting executable under terms of your choice, provided that
 * you also meet, for each linked independent module, the terms and conditions of
 * the license of that module.  An independent module is a module which is not
 * derived from this software.  The special exception does not apply to any
 * modifications of the software.
 * 
 *      Notwithstanding the above, under no circumstances may you combine this
 * software in any way with any other Broadcom software provided under a license
 * other than the GPL, without Broadcom's express prior written consent.
 *
 *
 * <<Broadcom-WL-IPTag/Proprietary,Open:>>
 *
 * $Id: bcmsdh_sdmmc.c 514727 2014-11-12 03:02:48Z $
 */
#include <typedefs.h>

#include <bcmdevs.h>
#include <bcmendian.h>
#include <bcmutils.h>
#include <osl.h>
#include <sdio.h>	
#include <sdioh.h>	
#include <bcmsdbus.h>	
#include <sdiovar.h>	

#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>

#include <dngl_stats.h>
#include <dhd.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 27)) && defined(CONFIG_PM_SLEEP)
#include <linux/suspend.h>
extern volatile bool dhd_mmc_suspend;
#endif
#include "bcmsdh_sdmmc.h"

#ifndef BCMSDH_MODULE
extern int sdio_function_init(void);
extern void sdio_function_cleanup(void);
#endif 

#if !defined(OOB_INTR_ONLY)
static void IRQHandler(struct sdio_func *func);
static void IRQHandlerF2(struct sdio_func *func);
#endif 
static int sdioh_sdmmc_get_cisaddr(sdioh_info_t *sd, uint32 regaddr);
extern int sdio_reset_comm(struct mmc_card *card);

#define DEFAULT_SDIO_F2_BLKSIZE		512
#ifndef CUSTOM_SDIO_F2_BLKSIZE
#define CUSTOM_SDIO_F2_BLKSIZE		DEFAULT_SDIO_F2_BLKSIZE
#endif

#define MAX_IO_RW_EXTENDED_BLK		511

uint sd_sdmode = SDIOH_MODE_SD4;	
uint sd_f2_blocksize = CUSTOM_SDIO_F2_BLKSIZE;
uint sd_divisor = 2;			

uint sd_power = 1;		
uint sd_clock = 1;		
uint sd_hiok = FALSE;	
uint sd_msglevel = 0x01;
uint sd_use_dma = TRUE;

#ifndef CUSTOM_RXCHAIN
#define CUSTOM_RXCHAIN 0
#endif

DHD_PM_RESUME_WAIT_INIT(sdioh_request_byte_wait);
DHD_PM_RESUME_WAIT_INIT(sdioh_request_word_wait);
DHD_PM_RESUME_WAIT_INIT(sdioh_request_packet_wait);
DHD_PM_RESUME_WAIT_INIT(sdioh_request_buffer_wait);

#define DMA_ALIGN_MASK	0x03
#define MMC_SDIO_ABORT_RETRY_LIMIT 5

int sdioh_sdmmc_card_regread(sdioh_info_t *sd, int func, uint32 regaddr, int regsize, uint32 *data);

static int
sdioh_sdmmc_card_enablefuncs(sdioh_info_t *sd)
{
	int err_ret;
	uint32 fbraddr;
	uint8 func;

	sd_trace(("%s\n", __FUNCTION__));

	
	sd->com_cis_ptr = sdioh_sdmmc_get_cisaddr(sd, SDIOD_CCCR_CISPTR_0);
	sd->func_cis_ptr[0] = sd->com_cis_ptr;
	sd_info(("%s: Card's Common CIS Ptr = 0x%x\n", __FUNCTION__, sd->com_cis_ptr));

	
	for (fbraddr = SDIOD_FBR_STARTADDR, func = 1;
	     func <= sd->num_funcs; func++, fbraddr += SDIOD_FBR_SIZE) {
		sd->func_cis_ptr[func] = sdioh_sdmmc_get_cisaddr(sd, SDIOD_FBR_CISPTR_0 + fbraddr);
		sd_info(("%s: Function %d CIS Ptr = 0x%x\n",
		         __FUNCTION__, func, sd->func_cis_ptr[func]));
	}

	sd->func_cis_ptr[0] = sd->com_cis_ptr;
	sd_info(("%s: Card's Common CIS Ptr = 0x%x\n", __FUNCTION__, sd->com_cis_ptr));

	
	sdio_claim_host(sd->func[1]);
	err_ret = sdio_enable_func(sd->func[1]);
	sdio_release_host(sd->func[1]);
	if (err_ret) {
		sd_err(("bcmsdh_sdmmc: Failed to enable F1 Err: 0x%08x", err_ret));
	}

	return FALSE;
}

extern sdioh_info_t *
sdioh_attach(osl_t *osh, struct sdio_func *func)
{
	sdioh_info_t *sd = NULL;
	int err_ret;

	sd_trace(("%s\n", __FUNCTION__));

	if (func == NULL) {
		sd_err(("%s: sdio function device is NULL\n", __FUNCTION__));
		return NULL;
	}

	if ((sd = (sdioh_info_t *)MALLOC(osh, sizeof(sdioh_info_t))) == NULL) {
		sd_err(("sdioh_attach: out of memory, malloced %d bytes\n", MALLOCED(osh)));
		return NULL;
	}
	bzero((char *)sd, sizeof(sdioh_info_t));
	sd->osh = osh;
	sd->fake_func0.num = 0;
	sd->fake_func0.card = func->card;
	sd->func[0] = &sd->fake_func0;
	sd->func[1] = func->card->sdio_func[0];
	sd->func[2] = func->card->sdio_func[1];
	sd->num_funcs = 2;
	sd->sd_blockmode = TRUE;
	sd->use_client_ints = TRUE;
	sd->client_block_size[0] = 64;
	sd->use_rxchain = CUSTOM_RXCHAIN;
	if (sd->func[1] == NULL || sd->func[2] == NULL) {
		sd_err(("%s: func 1 or 2 is null \n", __FUNCTION__));
		goto fail;
	}
	sdio_set_drvdata(sd->func[1], sd);

	sdio_claim_host(sd->func[1]);
	sd->client_block_size[1] = 64;
	err_ret = sdio_set_block_size(sd->func[1], 64);
	sdio_release_host(sd->func[1]);
	if (err_ret) {
		sd_err(("bcmsdh_sdmmc: Failed to set F1 blocksize(%d)\n", err_ret));
		goto fail;
	}

	sdio_claim_host(sd->func[2]);
	sd->client_block_size[2] = sd_f2_blocksize;
	err_ret = sdio_set_block_size(sd->func[2], sd_f2_blocksize);
	sdio_release_host(sd->func[2]);
	if (err_ret) {
		sd_err(("bcmsdh_sdmmc: Failed to set F2 blocksize to %d(%d)\n",
			sd_f2_blocksize, err_ret));
		goto fail;
	}

	sdioh_sdmmc_card_enablefuncs(sd);

	sd_trace(("%s: Done\n", __FUNCTION__));
	return sd;

fail:
	MFREE(sd->osh, sd, sizeof(sdioh_info_t));
	return NULL;
}


extern SDIOH_API_RC
sdioh_detach(osl_t *osh, sdioh_info_t *sd)
{
	sd_trace(("%s\n", __FUNCTION__));

	if (sd) {

		
		if (sd->func[2]) {
			sdio_claim_host(sd->func[2]);
			sdio_disable_func(sd->func[2]);
			sdio_release_host(sd->func[2]);
		}

		
		if (sd->func[1]) {
			sdio_claim_host(sd->func[1]);
			sdio_disable_func(sd->func[1]);
			sdio_release_host(sd->func[1]);
		}

		sd->func[1] = NULL;
		sd->func[2] = NULL;

		MFREE(sd->osh, sd, sizeof(sdioh_info_t));
	}
	return SDIOH_API_RC_SUCCESS;
}

#if defined(OOB_INTR_ONLY) && defined(HW_OOB)

extern SDIOH_API_RC
sdioh_enable_func_intr(sdioh_info_t *sd)
{
	uint8 reg;
	int err;

	if (sd->func[0] == NULL) {
		sd_err(("%s: function 0 pointer is NULL\n", __FUNCTION__));
		return SDIOH_API_RC_FAIL;
	}

	sdio_claim_host(sd->func[0]);
	reg = sdio_readb(sd->func[0], SDIOD_CCCR_INTEN, &err);
	if (err) {
		sd_err(("%s: error for read SDIO_CCCR_IENx : 0x%x\n", __FUNCTION__, err));
		sdio_release_host(sd->func[0]);
		return SDIOH_API_RC_FAIL;
	}
	
	reg &= ~INTR_CTL_MASTER_EN;
	reg |= (INTR_CTL_FUNC1_EN | INTR_CTL_FUNC2_EN);
	sdio_writeb(sd->func[0], reg, SDIOD_CCCR_INTEN, &err);
	sdio_release_host(sd->func[0]);

	if (err) {
		sd_err(("%s: error for write SDIO_CCCR_IENx : 0x%x\n", __FUNCTION__, err));
		return SDIOH_API_RC_FAIL;
	}

	return SDIOH_API_RC_SUCCESS;
}

extern SDIOH_API_RC
sdioh_disable_func_intr(sdioh_info_t *sd)
{
	uint8 reg;
	int err;

	if (sd->func[0] == NULL) {
		sd_err(("%s: function 0 pointer is NULL\n", __FUNCTION__));
		return SDIOH_API_RC_FAIL;
	}

	sdio_claim_host(sd->func[0]);
	reg = sdio_readb(sd->func[0], SDIOD_CCCR_INTEN, &err);
	if (err) {
		sd_err(("%s: error for read SDIO_CCCR_IENx : 0x%x\n", __FUNCTION__, err));
		sdio_release_host(sd->func[0]);
		return SDIOH_API_RC_FAIL;
	}
	reg &= ~(INTR_CTL_FUNC1_EN | INTR_CTL_FUNC2_EN);
	
	if (!(reg & 0xFE))
		reg = 0;
	sdio_writeb(sd->func[0], reg, SDIOD_CCCR_INTEN, &err);
	sdio_release_host(sd->func[0]);

	if (err) {
		sd_err(("%s: error for write SDIO_CCCR_IENx : 0x%x\n", __FUNCTION__, err));
		return SDIOH_API_RC_FAIL;
	}

	return SDIOH_API_RC_SUCCESS;
}
#endif 

extern SDIOH_API_RC
sdioh_interrupt_register(sdioh_info_t *sd, sdioh_cb_fn_t fn, void *argh)
{
	sd_trace(("%s: Entering\n", __FUNCTION__));
	if (fn == NULL) {
		sd_err(("%s: interrupt handler is NULL, not registering\n", __FUNCTION__));
		return SDIOH_API_RC_FAIL;
	}
#if !defined(OOB_INTR_ONLY)
	sd->intr_handler = fn;
	sd->intr_handler_arg = argh;
	sd->intr_handler_valid = TRUE;

	
	if (sd->func[2]) {
		sdio_claim_host(sd->func[2]);
		sdio_claim_irq(sd->func[2], IRQHandlerF2);
		sdio_release_host(sd->func[2]);
	}

	if (sd->func[1]) {
		sdio_claim_host(sd->func[1]);
		sdio_claim_irq(sd->func[1], IRQHandler);
		sdio_release_host(sd->func[1]);
	}
#elif defined(HW_OOB)
	sdioh_enable_func_intr(sd);
#endif 

	return SDIOH_API_RC_SUCCESS;
}

extern SDIOH_API_RC
sdioh_interrupt_deregister(sdioh_info_t *sd)
{
	sd_trace(("%s: Entering\n", __FUNCTION__));

#if !defined(OOB_INTR_ONLY)
	if (sd->func[1]) {
		
		sdio_claim_host(sd->func[1]);
		sdio_release_irq(sd->func[1]);
		sdio_release_host(sd->func[1]);
	}

	if (sd->func[2]) {
		
		sdio_claim_host(sd->func[2]);
		sdio_release_irq(sd->func[2]);
		
		sdio_release_host(sd->func[2]);
	}

	sd->intr_handler_valid = FALSE;
	sd->intr_handler = NULL;
	sd->intr_handler_arg = NULL;
#elif defined(HW_OOB)
	sdioh_disable_func_intr(sd);
#endif 
	return SDIOH_API_RC_SUCCESS;
}

extern SDIOH_API_RC
sdioh_interrupt_query(sdioh_info_t *sd, bool *onoff)
{
	sd_trace(("%s: Entering\n", __FUNCTION__));
	*onoff = sd->client_intr_enabled;
	return SDIOH_API_RC_SUCCESS;
}

#if defined(DHD_DEBUG)
extern bool
sdioh_interrupt_pending(sdioh_info_t *sd)
{
	return (0);
}
#endif

uint
sdioh_query_iofnum(sdioh_info_t *sd)
{
	return sd->num_funcs;
}

enum {
	IOV_MSGLEVEL = 1,
	IOV_BLOCKMODE,
	IOV_BLOCKSIZE,
	IOV_DMA,
	IOV_USEINTS,
	IOV_NUMINTS,
	IOV_NUMLOCALINTS,
	IOV_HOSTREG,
	IOV_DEVREG,
	IOV_DIVISOR,
	IOV_SDMODE,
	IOV_HISPEED,
	IOV_HCIREGS,
	IOV_POWER,
	IOV_CLOCK,
	IOV_RXCHAIN
};

const bcm_iovar_t sdioh_iovars[] = {
	{"sd_msglevel", IOV_MSGLEVEL,	0,	IOVT_UINT32,	0 },
	{"sd_blockmode", IOV_BLOCKMODE, 0,	IOVT_BOOL,	0 },
	{"sd_blocksize", IOV_BLOCKSIZE, 0,	IOVT_UINT32,	0 }, 
	{"sd_dma",	IOV_DMA,	0,	IOVT_BOOL,	0 },
	{"sd_ints", 	IOV_USEINTS,	0,	IOVT_BOOL,	0 },
	{"sd_numints",	IOV_NUMINTS,	0,	IOVT_UINT32,	0 },
	{"sd_numlocalints", IOV_NUMLOCALINTS, 0, IOVT_UINT32,	0 },
	{"sd_hostreg",	IOV_HOSTREG,	0,	IOVT_BUFFER,	sizeof(sdreg_t) },
	{"sd_devreg",	IOV_DEVREG, 	0,	IOVT_BUFFER,	sizeof(sdreg_t) },
	{"sd_divisor",	IOV_DIVISOR,	0,	IOVT_UINT32,	0 },
	{"sd_power",	IOV_POWER,	0,	IOVT_UINT32,	0 },
	{"sd_clock",	IOV_CLOCK,	0,	IOVT_UINT32,	0 },
	{"sd_mode", 	IOV_SDMODE, 	0,	IOVT_UINT32,	100},
	{"sd_highspeed", IOV_HISPEED,	0,	IOVT_UINT32,	0 },
	{"sd_rxchain",  IOV_RXCHAIN,    0, 	IOVT_BOOL,	0 },
	{NULL, 0, 0, 0, 0 }
};

int
sdioh_iovar_op(sdioh_info_t *si, const char *name,
                           void *params, int plen, void *arg, int len, bool set)
{
	const bcm_iovar_t *vi = NULL;
	int bcmerror = 0;
	int val_size;
	int32 int_val = 0;
	bool bool_val;
	uint32 actionid;

	ASSERT(name);
	ASSERT(len >= 0);

	
	ASSERT(set || (arg && len));
	ASSERT(!set || (!params && !plen));

	sd_trace(("%s: Enter (%s %s)\n", __FUNCTION__, (set ? "set" : "get"), name));

	if ((vi = bcm_iovar_lookup(sdioh_iovars, name)) == NULL) {
		bcmerror = BCME_UNSUPPORTED;
		goto exit;
	}

	if ((bcmerror = bcm_iovar_lencheck(vi, arg, len, set)) != 0)
		goto exit;

	
	if (params == NULL) {
		params = arg;
		plen = len;
	}

	if (vi->type == IOVT_VOID)
		val_size = 0;
	else if (vi->type == IOVT_BUFFER)
		val_size = len;
	else
		val_size = sizeof(int);

	if (plen >= (int)sizeof(int_val))
		bcopy(params, &int_val, sizeof(int_val));

	bool_val = (int_val != 0) ? TRUE : FALSE;
	BCM_REFERENCE(bool_val);

	actionid = set ? IOV_SVAL(vi->varid) : IOV_GVAL(vi->varid);
	switch (actionid) {
	case IOV_GVAL(IOV_MSGLEVEL):
		int_val = (int32)sd_msglevel;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_MSGLEVEL):
		sd_msglevel = int_val;
		break;

	case IOV_GVAL(IOV_BLOCKMODE):
		int_val = (int32)si->sd_blockmode;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_BLOCKMODE):
		si->sd_blockmode = (bool)int_val;
		
		break;

	case IOV_GVAL(IOV_BLOCKSIZE):
		if ((uint32)int_val > si->num_funcs) {
			bcmerror = BCME_BADARG;
			break;
		}
		int_val = (int32)si->client_block_size[int_val];
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_BLOCKSIZE):
	{
		uint func = ((uint32)int_val >> 16);
		uint blksize = (uint16)int_val;
		uint maxsize;

		if (func > si->num_funcs) {
			bcmerror = BCME_BADARG;
			break;
		}

		switch (func) {
		case 0: maxsize = 32; break;
		case 1: maxsize = BLOCK_SIZE_4318; break;
		case 2: maxsize = BLOCK_SIZE_4328; break;
		default: maxsize = 0;
		}
		if (blksize > maxsize) {
			bcmerror = BCME_BADARG;
			break;
		}
		if (!blksize) {
			blksize = maxsize;
		}

		
		si->client_block_size[func] = blksize;

		break;
	}

	case IOV_GVAL(IOV_RXCHAIN):
		int_val = (int32)si->use_rxchain;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_DMA):
		int_val = (int32)si->sd_use_dma;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_DMA):
		si->sd_use_dma = (bool)int_val;
		break;

	case IOV_GVAL(IOV_USEINTS):
		int_val = (int32)si->use_client_ints;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_USEINTS):
		si->use_client_ints = (bool)int_val;
		if (si->use_client_ints)
			si->intmask |= CLIENT_INTR;
		else
			si->intmask &= ~CLIENT_INTR;

		break;

	case IOV_GVAL(IOV_DIVISOR):
		int_val = (uint32)sd_divisor;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_DIVISOR):
		sd_divisor = int_val;
		break;

	case IOV_GVAL(IOV_POWER):
		int_val = (uint32)sd_power;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_POWER):
		sd_power = int_val;
		break;

	case IOV_GVAL(IOV_CLOCK):
		int_val = (uint32)sd_clock;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_CLOCK):
		sd_clock = int_val;
		break;

	case IOV_GVAL(IOV_SDMODE):
		int_val = (uint32)sd_sdmode;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_SDMODE):
		sd_sdmode = int_val;
		break;

	case IOV_GVAL(IOV_HISPEED):
		int_val = (uint32)sd_hiok;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_SVAL(IOV_HISPEED):
		sd_hiok = int_val;
		break;

	case IOV_GVAL(IOV_NUMINTS):
		int_val = (int32)si->intrcount;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_NUMLOCALINTS):
		int_val = (int32)0;
		bcopy(&int_val, arg, val_size);
		break;

	case IOV_GVAL(IOV_HOSTREG):
	{
		sdreg_t *sd_ptr = (sdreg_t *)params;

		if (sd_ptr->offset < SD_SysAddr || sd_ptr->offset > SD_MaxCurCap) {
			sd_err(("%s: bad offset 0x%x\n", __FUNCTION__, sd_ptr->offset));
			bcmerror = BCME_BADARG;
			break;
		}

		sd_trace(("%s: rreg%d at offset %d\n", __FUNCTION__,
		                  (sd_ptr->offset & 1) ? 8 : ((sd_ptr->offset & 2) ? 16 : 32),
		                  sd_ptr->offset));
		if (sd_ptr->offset & 1)
			int_val = 8; 
		else if (sd_ptr->offset & 2)
			int_val = 16; 
		else
			int_val = 32; 

		bcopy(&int_val, arg, sizeof(int_val));
		break;
	}

	case IOV_SVAL(IOV_HOSTREG):
	{
		sdreg_t *sd_ptr = (sdreg_t *)params;

		if (sd_ptr->offset < SD_SysAddr || sd_ptr->offset > SD_MaxCurCap) {
			sd_err(("%s: bad offset 0x%x\n", __FUNCTION__, sd_ptr->offset));
			bcmerror = BCME_BADARG;
			break;
		}

		sd_trace(("%s: wreg%d value 0x%08x at offset %d\n", __FUNCTION__, sd_ptr->value,
		                  (sd_ptr->offset & 1) ? 8 : ((sd_ptr->offset & 2) ? 16 : 32),
		                  sd_ptr->offset));
		break;
	}

	case IOV_GVAL(IOV_DEVREG):
	{
		sdreg_t *sd_ptr = (sdreg_t *)params;
		uint8 data = 0;

		if (sdioh_cfg_read(si, sd_ptr->func, sd_ptr->offset, &data)) {
			bcmerror = BCME_SDIO_ERROR;
			break;
		}

		int_val = (int)data;
		bcopy(&int_val, arg, sizeof(int_val));
		break;
	}

	case IOV_SVAL(IOV_DEVREG):
	{
		sdreg_t *sd_ptr = (sdreg_t *)params;
		uint8 data = (uint8)sd_ptr->value;

		if (sdioh_cfg_write(si, sd_ptr->func, sd_ptr->offset, &data)) {
			bcmerror = BCME_SDIO_ERROR;
			break;
		}
		break;
	}

	default:
		bcmerror = BCME_UNSUPPORTED;
		break;
	}
exit:

	return bcmerror;
}

#if defined(OOB_INTR_ONLY) && defined(HW_OOB)

SDIOH_API_RC
sdioh_enable_hw_oob_intr(sdioh_info_t *sd, bool enable)
{
	SDIOH_API_RC status;
	uint8 data;

	if (enable)
		data = SDIO_SEPINT_MASK | SDIO_SEPINT_OE | SDIO_SEPINT_ACT_HI;
	else
		data = SDIO_SEPINT_ACT_HI;	

	status = sdioh_request_byte(sd, SDIOH_WRITE, 0, SDIOD_CCCR_BRCM_SEPINT, &data);
	return status;
}
#endif 

extern SDIOH_API_RC
sdioh_cfg_read(sdioh_info_t *sd, uint fnc_num, uint32 addr, uint8 *data)
{
	SDIOH_API_RC status;
	
	status = sdioh_request_byte(sd, SDIOH_READ, fnc_num, addr, data);
	return status;
}

extern SDIOH_API_RC
sdioh_cfg_write(sdioh_info_t *sd, uint fnc_num, uint32 addr, uint8 *data)
{
	
	SDIOH_API_RC status;
	status = sdioh_request_byte(sd, SDIOH_WRITE, fnc_num, addr, data);
	return status;
}

static int
sdioh_sdmmc_get_cisaddr(sdioh_info_t *sd, uint32 regaddr)
{
	
	int i;
	uint32 scratch, regdata;
	uint8 *ptr = (uint8 *)&scratch;
	for (i = 0; i < 3; i++) {
		if ((sdioh_sdmmc_card_regread (sd, 0, regaddr, 1, &regdata)) != SUCCESS)
			sd_err(("%s: Can't read!\n", __FUNCTION__));

		*ptr++ = (uint8) regdata;
		regaddr++;
	}

	
	scratch = ltoh32(scratch);
	scratch &= 0x0001FFFF;
	return (scratch);
}

extern SDIOH_API_RC
sdioh_cis_read(sdioh_info_t *sd, uint func, uint8 *cisd, uint32 length)
{
	uint32 count;
	int offset;
	uint32 foo;
	uint8 *cis = cisd;

	sd_trace(("%s: Func = %d\n", __FUNCTION__, func));

	if (!sd->func_cis_ptr[func]) {
		bzero(cis, length);
		sd_err(("%s: no func_cis_ptr[%d]\n", __FUNCTION__, func));
		return SDIOH_API_RC_FAIL;
	}

	sd_err(("%s: func_cis_ptr[%d]=0x%04x\n", __FUNCTION__, func, sd->func_cis_ptr[func]));

	for (count = 0; count < length; count++) {
		offset =  sd->func_cis_ptr[func] + count;
		if (sdioh_sdmmc_card_regread (sd, 0, offset, 1, &foo) < 0) {
			sd_err(("%s: regread failed: Can't read CIS\n", __FUNCTION__));
			return SDIOH_API_RC_FAIL;
		}

		*cis = (uint8)(foo & 0xff);
		cis++;
	}

	return SDIOH_API_RC_SUCCESS;
}

extern SDIOH_API_RC
sdioh_request_byte(sdioh_info_t *sd, uint rw, uint func, uint regaddr, uint8 *byte)
{
	int err_ret = 0;
#if defined(MMC_SDIO_ABORT)
	int sdio_abort_retry = MMC_SDIO_ABORT_RETRY_LIMIT;
#endif

	sd_info(("%s: rw=%d, func=%d, addr=0x%05x\n", __FUNCTION__, rw, func, regaddr));

	DHD_PM_RESUME_WAIT(sdioh_request_byte_wait);
	DHD_PM_RESUME_RETURN_ERROR(SDIOH_API_RC_FAIL);
	if(rw) { 
		if (func == 0) {
			if (regaddr == SDIOD_CCCR_IOEN) {
				if (sd->func[2]) {
					sdio_claim_host(sd->func[2]);
					if (*byte & SDIO_FUNC_ENABLE_2) {
						
						err_ret = sdio_enable_func(sd->func[2]);
						if (err_ret) {
							sd_err(("bcmsdh_sdmmc: enable F2 failed:%d",
								err_ret));
						}
					} else {
						
						err_ret = sdio_disable_func(sd->func[2]);
						if (err_ret) {
							sd_err(("bcmsdh_sdmmc: Disab F2 failed:%d",
								err_ret));
						}
					}
					sdio_release_host(sd->func[2]);
				}
			}
#if defined(MMC_SDIO_ABORT)
			
			else if (regaddr == SDIOD_CCCR_IOABORT) {
				while (sdio_abort_retry--) {
					if (sd->func[func]) {
						sdio_claim_host(sd->func[func]);
						sdio_writeb(sd->func[func],
							*byte, regaddr, &err_ret);
						sdio_release_host(sd->func[func]);
					}
					if (!err_ret)
						break;
				}
			}
#endif 
			else if (regaddr < 0xF0) {
				sd_err(("bcmsdh_sdmmc: F0 Wr:0x%02x: write disallowed\n", regaddr));
			} else {
				
				if (sd->func[func]) {
					sdio_claim_host(sd->func[func]);
					sdio_f0_writeb(sd->func[func],
						*byte, regaddr, &err_ret);
					sdio_release_host(sd->func[func]);
				}
			}
		} else {
			
			if (sd->func[func]) {
				sdio_claim_host(sd->func[func]);
				sdio_writeb(sd->func[func], *byte, regaddr, &err_ret);
				sdio_release_host(sd->func[func]);
			}
		}
	} else { 
		
		if (sd->func[func]) {
			sdio_claim_host(sd->func[func]);
			if (func == 0) {
				*byte = sdio_f0_readb(sd->func[func], regaddr, &err_ret);
			} else {
				*byte = sdio_readb(sd->func[func], regaddr, &err_ret);
			}
			sdio_release_host(sd->func[func]);
		}
	}

	if (err_ret) {
		if ((regaddr == 0x1001F) && ((err_ret == -ETIMEDOUT) || (err_ret == -EILSEQ))) {
		} else {
			sd_err(("bcmsdh_sdmmc: Failed to %s byte F%d:@0x%05x=%02x, Err: %d\n",
				rw ? "Write" : "Read", func, regaddr, *byte, err_ret));
		}
	}

	return ((err_ret == 0) ? SDIOH_API_RC_SUCCESS : SDIOH_API_RC_FAIL);
}

extern SDIOH_API_RC
sdioh_request_word(sdioh_info_t *sd, uint cmd_type, uint rw, uint func, uint addr,
                                   uint32 *word, uint nbytes)
{
	int err_ret = SDIOH_API_RC_FAIL;
#if defined(MMC_SDIO_ABORT)
	int sdio_abort_retry = MMC_SDIO_ABORT_RETRY_LIMIT;
#endif

	if (func == 0) {
		sd_err(("%s: Only CMD52 allowed to F0.\n", __FUNCTION__));
		return SDIOH_API_RC_FAIL;
	}

	sd_info(("%s: cmd_type=%d, rw=%d, func=%d, addr=0x%05x, nbytes=%d\n",
	         __FUNCTION__, cmd_type, rw, func, addr, nbytes));

	DHD_PM_RESUME_WAIT(sdioh_request_word_wait);
	DHD_PM_RESUME_RETURN_ERROR(SDIOH_API_RC_FAIL);
	
	sdio_claim_host(sd->func[func]);

	if(rw) { 
		if (nbytes == 4) {
			sdio_writel(sd->func[func], *word, addr, &err_ret);
		} else if (nbytes == 2) {
			sdio_writew(sd->func[func], (*word & 0xFFFF), addr, &err_ret);
		} else {
			sd_err(("%s: Invalid nbytes: %d\n", __FUNCTION__, nbytes));
		}
	} else { 
		if (nbytes == 4) {
			*word = sdio_readl(sd->func[func], addr, &err_ret);
		} else if (nbytes == 2) {
			*word = sdio_readw(sd->func[func], addr, &err_ret) & 0xFFFF;
		} else {
			sd_err(("%s: Invalid nbytes: %d\n", __FUNCTION__, nbytes));
		}
	}

	
	sdio_release_host(sd->func[func]);

	if (err_ret) {
#if defined(MMC_SDIO_ABORT)
		
		while (sdio_abort_retry--) {
			if (sd->func[0]) {
				sdio_claim_host(sd->func[0]);
				sdio_writeb(sd->func[0],
					func, SDIOD_CCCR_IOABORT, &err_ret);
				sdio_release_host(sd->func[0]);
			}
			if (!err_ret)
				break;
		}
		if (err_ret)
#endif 
		{
			sd_err(("bcmsdh_sdmmc: Failed to %s word, Err: 0x%08x",
				rw ? "Write" : "Read", err_ret));
		}
	}

	return ((err_ret == 0) ? SDIOH_API_RC_SUCCESS : SDIOH_API_RC_FAIL);
}

static SDIOH_API_RC
sdioh_request_packet_chain(sdioh_info_t *sd, uint fix_inc, uint write, uint func,
                     uint addr, void *pkt)
{
	bool fifo = (fix_inc == SDIOH_DATA_FIX);
	int err_ret = 0;
	void *pnext;
	uint ttl_len, pkt_offset;
	uint blk_num;
	uint blk_size;
	uint max_blk_count;
	uint max_req_size;
	struct mmc_request mmc_req;
	struct mmc_command mmc_cmd;
	struct mmc_data mmc_dat;
	uint32 sg_count;
	struct sdio_func *sdio_func = sd->func[func];
	struct mmc_host *host = sdio_func->card->host;

	sd_trace(("%s: Enter\n", __FUNCTION__));
	ASSERT(pkt);
	DHD_PM_RESUME_WAIT(sdioh_request_packet_wait);
	DHD_PM_RESUME_RETURN_ERROR(SDIOH_API_RC_FAIL);

	blk_size = sd->client_block_size[func];
	max_blk_count = min(host->max_blk_count, (uint)MAX_IO_RW_EXTENDED_BLK);
	max_req_size = min(max_blk_count * blk_size, host->max_req_size);

	pkt_offset = 0;
	pnext = pkt;

	while (pnext != NULL) {
		ttl_len = 0;
		sg_count = 0;
		memset(&mmc_req, 0, sizeof(struct mmc_request));
		memset(&mmc_cmd, 0, sizeof(struct mmc_command));
		memset(&mmc_dat, 0, sizeof(struct mmc_data));
		sg_init_table(sd->sg_list, ARRAYSIZE(sd->sg_list));

		while (pnext != NULL && ttl_len < max_req_size) {
			int pkt_len;
			int sg_data_size;
			uint8 *pdata = (uint8*)PKTDATA(sd->osh, pnext);

			ASSERT(pdata != NULL);
			pkt_len = PKTLEN(sd->osh, pnext);
			sd_trace(("%s[%d] data=%p, len=%d\n", __FUNCTION__, write, pdata, pkt_len));
			if (sg_count >= ARRAYSIZE(sd->sg_list)) {
				sd_err(("%s: sg list entries exceed limit\n", __FUNCTION__));
				return (SDIOH_API_RC_FAIL);
			}
			pdata += pkt_offset;

			sg_data_size = pkt_len - pkt_offset;
			if (sg_data_size > max_req_size - ttl_len)
				sg_data_size = max_req_size - ttl_len;
			if (sg_data_size > host->max_seg_size)
				sg_data_size = host->max_seg_size;
			sg_set_buf(&sd->sg_list[sg_count++], pdata, sg_data_size);

			ttl_len += sg_data_size;
			pkt_offset += sg_data_size;
			if (pkt_offset == pkt_len) {
				pnext = PKTNEXT(sd->osh, pnext);
				pkt_offset = 0;
			}
		}

		if (ttl_len % blk_size != 0) {
			sd_err(("%s, data length %d not aligned to block size %d\n",
				__FUNCTION__,  ttl_len, blk_size));
			return SDIOH_API_RC_FAIL;
		}
		blk_num = ttl_len / blk_size;
		mmc_dat.sg = sd->sg_list;
		mmc_dat.sg_len = sg_count;
		mmc_dat.blksz = blk_size;
		mmc_dat.blocks = blk_num;
		mmc_dat.flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;
		mmc_cmd.opcode = 53; 
		mmc_cmd.arg = write ? 1<<31 : 0;
		mmc_cmd.arg |= (func & 0x7) << 28;
		mmc_cmd.arg |= 1<<27;
		mmc_cmd.arg |= fifo ? 0 : 1<<26;
		mmc_cmd.arg |= (addr & 0x1FFFF) << 9;
		mmc_cmd.arg |= blk_num & 0x1FF;
		mmc_cmd.flags = MMC_RSP_SPI_R5 | MMC_RSP_R5 | MMC_CMD_ADTC;
		mmc_req.cmd = &mmc_cmd;
		mmc_req.data = &mmc_dat;
		if (!fifo)
			addr += ttl_len;

		sdio_claim_host(sdio_func);
		mmc_set_data_timeout(&mmc_dat, sdio_func->card);
		mmc_wait_for_req(host, &mmc_req);
		sdio_release_host(sdio_func);

		err_ret = mmc_cmd.error? mmc_cmd.error : mmc_dat.error;
		if (0 != err_ret) {
			sd_err(("%s:CMD53 %s failed with code %d\n",
				__FUNCTION__, write ? "write" : "read", err_ret));
			return SDIOH_API_RC_FAIL;
		}
	}

	sd_trace(("%s: Exit\n", __FUNCTION__));
	return SDIOH_API_RC_SUCCESS;
}

static SDIOH_API_RC
sdioh_buffer_tofrom_bus(sdioh_info_t *sd, uint fix_inc, uint write, uint func,
                     uint addr, uint8 *buf, uint len)
{
	bool fifo = (fix_inc == SDIOH_DATA_FIX);
	int err_ret = 0;

	sd_trace(("%s: Enter\n", __FUNCTION__));
	ASSERT(buf);

	sdio_claim_host(sd->func[func]);

	if ((write) && (!fifo))
		err_ret = sdio_memcpy_toio(sd->func[func], addr, buf, len);
	else if (write)
		err_ret = sdio_memcpy_toio(sd->func[func], addr, buf, len);
	else if (fifo)
		err_ret = sdio_readsb(sd->func[func], buf, addr, len);
	else
		err_ret = sdio_memcpy_fromio(sd->func[func], buf, addr, len);

	sdio_release_host(sd->func[func]);

	if (err_ret)
		sd_err(("%s: %s FAILED %p, addr=0x%05x, pkt_len=%d, ERR=%d\n", __FUNCTION__,
		       (write) ? "TX" : "RX", buf, addr, len, err_ret));
	else
		sd_trace(("%s: %s xfr'd %p, addr=0x%05x, len=%d\n", __FUNCTION__,
			(write) ? "TX" : "RX", buf, addr, len));

	sd_trace(("%s: Exit\n", __FUNCTION__));
	return ((err_ret == 0) ? SDIOH_API_RC_SUCCESS : SDIOH_API_RC_FAIL);
}


extern SDIOH_API_RC
sdioh_request_buffer(sdioh_info_t *sd, uint pio_dma, uint fix_inc, uint write, uint func,
	uint addr, uint reg_width, uint buf_len, uint8 *buffer, void *pkt)
{
	SDIOH_API_RC status;
	void *tmppkt;

	sd_trace(("%s: Enter\n", __FUNCTION__));
	DHD_PM_RESUME_WAIT(sdioh_request_buffer_wait);
	DHD_PM_RESUME_RETURN_ERROR(SDIOH_API_RC_FAIL);

	if (pkt) {
		if (PKTNEXT(sd->osh, pkt))
			return sdioh_request_packet_chain(sd, fix_inc, write, func, addr, pkt);

		buffer = PKTDATA(sd->osh, pkt);
		buf_len = PKTLEN(sd->osh, pkt);
	}

	ASSERT(buffer);

	
	if (((ulong)buffer & DMA_ALIGN_MASK) == 0 && (buf_len & DMA_ALIGN_MASK) == 0)
		return sdioh_buffer_tofrom_bus(sd, fix_inc, write, func, addr, buffer, buf_len);

	sd_err(("%s: [%d] doing memory copy buf=%p, len=%d\n",
		__FUNCTION__, write, buffer, buf_len));

	
	tmppkt = PKTGET_STATIC(sd->osh, buf_len + DEFAULT_SDIO_F2_BLKSIZE, write ? TRUE : FALSE);
	if (tmppkt == NULL) {
		sd_err(("%s: PKTGET failed: len %d\n", __FUNCTION__, buf_len));
		return SDIOH_API_RC_FAIL;
	}

	if (write)
		bcopy(buffer, PKTDATA(sd->osh, tmppkt), buf_len);

	status = sdioh_buffer_tofrom_bus(sd, fix_inc, write, func, addr,
		PKTDATA(sd->osh, tmppkt), ROUNDUP(buf_len, (DMA_ALIGN_MASK+1)));

	if (!write)
		bcopy(PKTDATA(sd->osh, tmppkt), buffer, buf_len);

	PKTFREE_STATIC(sd->osh, tmppkt, write ? TRUE : FALSE);

	return status;
}

extern int
sdioh_abort(sdioh_info_t *sd, uint func)
{
#if defined(MMC_SDIO_ABORT)
	char t_func = (char) func;
#endif 
	sd_trace(("%s: Enter\n", __FUNCTION__));

#if defined(MMC_SDIO_ABORT)
	
	sdioh_request_byte(sd, SD_IO_OP_WRITE, SDIO_FUNC_0, SDIOD_CCCR_IOABORT, &t_func);
#endif 

	sd_trace(("%s: Exit\n", __FUNCTION__));
	return SDIOH_API_RC_SUCCESS;
}

int sdioh_sdio_reset(sdioh_info_t *si)
{
	sd_trace(("%s: Enter\n", __FUNCTION__));
	sd_trace(("%s: Exit\n", __FUNCTION__));
	return SDIOH_API_RC_SUCCESS;
}

void
sdioh_sdmmc_devintr_off(sdioh_info_t *sd)
{
	sd_trace(("%s: %d\n", __FUNCTION__, sd->use_client_ints));
	sd->intmask &= ~CLIENT_INTR;
}

void
sdioh_sdmmc_devintr_on(sdioh_info_t *sd)
{
	sd_trace(("%s: %d\n", __FUNCTION__, sd->use_client_ints));
	sd->intmask |= CLIENT_INTR;
}

int
sdioh_sdmmc_card_regread(sdioh_info_t *sd, int func, uint32 regaddr, int regsize, uint32 *data)
{

	if ((func == 0) || (regsize == 1)) {
		uint8 temp = 0;

		sdioh_request_byte(sd, SDIOH_READ, func, regaddr, &temp);
		*data = temp;
		*data &= 0xff;
		sd_data(("%s: byte read data=0x%02x\n",
		         __FUNCTION__, *data));
	} else {
		sdioh_request_word(sd, 0, SDIOH_READ, func, regaddr, data, regsize);
		if (regsize == 2)
			*data &= 0xffff;

		sd_data(("%s: word read data=0x%08x\n",
		         __FUNCTION__, *data));
	}

	return SUCCESS;
}

#if !defined(OOB_INTR_ONLY)
static void IRQHandler(struct sdio_func *func)
{
	sdioh_info_t *sd;

	sd = sdio_get_drvdata(func);

	ASSERT(sd != NULL);
	sdio_release_host(sd->func[0]);

	if (sd->use_client_ints) {
		sd->intrcount++;
		ASSERT(sd->intr_handler);
		ASSERT(sd->intr_handler_arg);
		(sd->intr_handler)(sd->intr_handler_arg);
	} else {
		sd_err(("bcmsdh_sdmmc: ***IRQHandler\n"));

		sd_err(("%s: Not ready for intr: enabled %d, handler %p\n",
		        __FUNCTION__, sd->client_intr_enabled, sd->intr_handler));
	}

	sdio_claim_host(sd->func[0]);
}

static void IRQHandlerF2(struct sdio_func *func)
{
	sd_trace(("bcmsdh_sdmmc: ***IRQHandlerF2\n"));
}
#endif 

#ifdef NOTUSED
static int
sdioh_sdmmc_card_regwrite(sdioh_info_t *sd, int func, uint32 regaddr, int regsize, uint32 data)
{

	if ((func == 0) || (regsize == 1)) {
		uint8 temp;

		temp = data & 0xff;
		sdioh_request_byte(sd, SDIOH_READ, func, regaddr, &temp);
		sd_data(("%s: byte write data=0x%02x\n",
		         __FUNCTION__, data));
	} else {
		if (regsize == 2)
			data &= 0xffff;

		sdioh_request_word(sd, 0, SDIOH_READ, func, regaddr, &data, regsize);

		sd_data(("%s: word write data=0x%08x\n",
		         __FUNCTION__, data));
	}

	return SUCCESS;
}
#endif 

int
sdioh_start(sdioh_info_t *sd, int stage)
{
	int ret;

	if (!sd) {
		sd_err(("%s Failed, sd is NULL\n", __FUNCTION__));
		return (0);
	}

	if (sd->func[0]) {
			if (stage == 0) {
		if ((ret = sdio_reset_comm(sd->func[0]->card))) {
			sd_err(("%s Failed, error = %d\n", __FUNCTION__, ret));
			return ret;
		}
		else {
			sd->num_funcs = 2;
			sd->sd_blockmode = TRUE;
			sd->use_client_ints = TRUE;
			sd->client_block_size[0] = 64;

			if (sd->func[1]) {
				
				sdio_claim_host(sd->func[1]);

				sd->client_block_size[1] = 64;
				ret = sdio_set_block_size(sd->func[1], 64);
				if (ret) {
					sd_err(("bcmsdh_sdmmc: Failed to set F1 "
						"blocksize(%d)\n", ret));
				}

				
				sdio_release_host(sd->func[1]);
			}

			if (sd->func[2]) {
				
				sdio_claim_host(sd->func[2]);

				sd->client_block_size[2] = sd_f2_blocksize;
				ret = sdio_set_block_size(sd->func[2], sd_f2_blocksize);
				if (ret) {
					sd_err(("bcmsdh_sdmmc: Failed to set F2 "
						"blocksize to %d(%d)\n", sd_f2_blocksize, ret));
				}

				
				sdio_release_host(sd->func[2]);
			}

			sdioh_sdmmc_card_enablefuncs(sd);
			}
		} else {
#if !defined(OOB_INTR_ONLY)
			sdio_claim_host(sd->func[0]);
			if (sd->func[2])
				sdio_claim_irq(sd->func[2], IRQHandlerF2);
			if (sd->func[1])
				sdio_claim_irq(sd->func[1], IRQHandler);
			sdio_release_host(sd->func[0]);
#else 
#if defined(HW_OOB)
			sdioh_enable_func_intr(sd);
#endif
			bcmsdh_oob_intr_set(sd->bcmsdh, TRUE);
#endif 
		}
	}
	else
		sd_err(("%s Failed\n", __FUNCTION__));

	return (0);
}

int
sdioh_stop(sdioh_info_t *sd)
{
	if (sd->func[0]) {
#if !defined(OOB_INTR_ONLY)
		sdio_claim_host(sd->func[0]);
		if (sd->func[1])
			sdio_release_irq(sd->func[1]);
		if (sd->func[2])
			sdio_release_irq(sd->func[2]);
		sdio_release_host(sd->func[0]);
#else 
#if defined(HW_OOB)
		sdioh_disable_func_intr(sd);
#endif
		bcmsdh_oob_intr_set(sd->bcmsdh, FALSE);
#endif 
	}
	else
		sd_err(("%s Failed\n", __FUNCTION__));
	return (0);
}

int
sdioh_waitlockfree(sdioh_info_t *sd)
{
	return (1);
}


SDIOH_API_RC
sdioh_gpioouten(sdioh_info_t *sd, uint32 gpio)
{
	return SDIOH_API_RC_FAIL;
}

SDIOH_API_RC
sdioh_gpioout(sdioh_info_t *sd, uint32 gpio, bool enab)
{
	return SDIOH_API_RC_FAIL;
}

bool
sdioh_gpioin(sdioh_info_t *sd, uint32 gpio)
{
	return FALSE;
}

SDIOH_API_RC
sdioh_gpio_init(sdioh_info_t *sd)
{
	return SDIOH_API_RC_FAIL;
}
