/******************************************************************************
* Copyright (c) 2018 - 2022 Xilinx, Inc.  All rights reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/


#include "xil_util.h"
#include "xpm_pll.h"
#include "xpm_psm.h"
#include "xpm_regs.h"

static struct XPm_PllTopology PllTopologies[] =
{
	{ TOPOLOGY_GENERIC_PLL, PLLPARAMS, RESET_SHIFT, BYPASS_SHIFT,
	  GEN_LOCK_SHIFT, GEN_STABLE_SHIFT, GEN_REG3_OFFSET },
	{ TOPOLOGY_NOC_PLL, PLLPARAMS, RESET_SHIFT, BYPASS_SHIFT,
	  NPLL_LOCK_SHIFT, NPLL_STABLE_SHIFT, NPLL_REG3_OFFSET },
};

XStatus XPmClockPll_AddNode(u32 Id, u32 ControlReg, u8 TopologyType,
			    const u16 *Offsets, u32 PowerDomainId, u8 ClkFlags)
{
	XStatus Status = XST_FAILURE;
	XPm_PllClockNode *PllClkPtr;

	if (NULL != XPmClock_GetById(Id)) {
		Status = XPM_PM_INVALID_NODE;
		goto done;
	}
	if ((TopologyType != TOPOLOGY_GENERIC_PLL) &&
	    (TopologyType != TOPOLOGY_NOC_PLL)) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	PllClkPtr = XPm_AllocBytes(sizeof(XPm_PllClockNode));
	if (PllClkPtr == NULL) {
		Status = XST_BUFFER_TOO_SMALL;
		goto done;
	}
	XPmNode_Init(&PllClkPtr->ClkNode.Node, Id, (u8)PM_PLL_STATE_SUSPENDED, 0);
	PllClkPtr->ClkNode.Node.BaseAddress = ControlReg;
	PllClkPtr->ClkNode.ClkHandles = NULL;
	PllClkPtr->ClkNode.UseCount = 0;
	PllClkPtr->ClkNode.NumParents = 1;
	PllClkPtr->ClkNode.Flags = ClkFlags;
	PllClkPtr->Topology = &PllTopologies[TopologyType-TOPOLOGY_GENERIC_PLL];
	PllClkPtr->StatusReg = ControlReg + Offsets[0];
	PllClkPtr->ConfigReg = ControlReg + Offsets[1];
	PllClkPtr->FracConfigReg = ControlReg + Offsets[2];

	Status = XPmClock_SetById(Id, (XPm_ClockNode *)PllClkPtr);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	if (((u32)XPM_NODECLASS_POWER != NODECLASS(PowerDomainId)) ||
	    ((u32)XPM_NODESUBCL_POWER_DOMAIN != NODESUBCLASS(PowerDomainId))) {
		PllClkPtr->ClkNode.PwrDomain = NULL;
		goto done;
	}

	PllClkPtr->ClkNode.PwrDomain = XPmPower_GetById(PowerDomainId);
	if (NULL == PllClkPtr->ClkNode.PwrDomain) {
		Status = XST_DEVICE_NOT_FOUND;
		goto done;
	}

done:
	return Status;
}

XStatus XPmClockPll_AddParent(u32 Id, const u32 *Parents, u8 NumParents)
{
	XStatus Status = XST_FAILURE;
	XPm_PllClockNode *PllPtr = (XPm_PllClockNode *)XPmClock_GetById(Id);

	if (PllPtr == NULL) {
		Status = XST_INVALID_PARAM;
		goto done;
	}
	if ((PllPtr->ClkNode.NumParents == 1U) && (NumParents != 1U)) {
		Status = XST_INVALID_PARAM;
		goto done;
	} else {
		PllPtr->ClkNode.ParentIdx = (u16)(NODEINDEX(Parents[0]));
		Status = XST_SUCCESS;
	}

done:
	return Status;
}

static void XPm_PllClearLockError(const XPm_PllClockNode* Pll)
{
	const XPm_Psm *Psm = (XPm_Psm *)XPmDevice_GetById(PM_DEV_PSM_PROC);
	if (NULL != Psm) {
		if (PM_CLK_APLL1 == Pll->ClkNode.Node.Id) {
			XPm_Write32(Psm->PsmGlobalBaseAddr + PSM_ERR1_STATUS_OFFSET,
				    PSM_ERR1_STATUS_APLL1_LOCK_MASK);
		} else if (PM_CLK_APLL2 == Pll->ClkNode.Node.Id) {
			XPm_Write32(Psm->PsmGlobalBaseAddr + PSM_ERR1_STATUS_OFFSET,
				    PSM_ERR1_STATUS_APLL2_LOCK_MASK);
		} else if (PM_CLK_RPLL == Pll->ClkNode.Node.Id) {
			XPm_Write32(Psm->PsmGlobalBaseAddr + PSM_ERR1_STATUS_OFFSET,
				    PSM_ERR1_STATUS_RPLL_LOCK_MASK);
		} else if (PM_CLK_FLXPLL == Pll->ClkNode.Node.Id) {
			XPm_Write32(Psm->PsmGlobalBaseAddr + PSM_ERR1_STATUS_OFFSET,
				    PSM_ERR1_STATUS_FLXPLL_LOCK_MASK);
		} else {
			/* Required due to MISRA */
		}
	}
}

XStatus XPmClockPll_Reset(XPm_PllClockNode *Pll, uint8_t Flags)
{
	XStatus Status = XST_FAILURE;
	u32 ControlReg = Pll->ClkNode.Node.BaseAddress;

	if (0U != (Flags & PLL_RESET_ASSERT)) {
		/* Bypass PLL before putting it into the reset */
		XPm_RMW32(ControlReg, BIT32(Pll->Topology->BypassShift),
			   BIT32(Pll->Topology->BypassShift));

		/* Power down PLL (= reset PLL) */
		XPm_RMW32(ControlReg, BIT32(Pll->Topology->ResetShift),
			   BIT32(Pll->Topology->ResetShift));
		Pll->ClkNode.Node.State = PM_PLL_STATE_RESET;
	}
	if (0U != (Flags & PLL_RESET_RELEASE)) {
		/* Deassert the reset */
		XPm_RMW32(ControlReg, BIT32(Pll->Topology->ResetShift),
			   ~BIT32(Pll->Topology->ResetShift));
		/* Poll status register for the lock */
		Status = XPm_PollForMask(Pll->StatusReg, BIT32(Pll->Topology->LockShift),
						  PLL_LOCK_TIMEOUT);
		/* Deassert bypass if the PLL locked */
		if (XST_SUCCESS == Status) {
			XPm_RMW32(ControlReg, BIT32(Pll->Topology->BypassShift),
					~BIT32(Pll->Topology->BypassShift));
			Pll->ClkNode.Node.State = PM_PLL_STATE_LOCKED;

			/**
			 * PLL lock error source needs to be disabled before PLL suspend
			 * and re-enable after PLL lock which can be done by disabling
			 * interrupt from PMC global module but it is also disabling
			 * another error interrupts. So another way is to clear the PLL
			 * error lock status once PLL is locked after resume.
			 */
			XPm_PllClearLockError(Pll);
		} else {
			goto done;
		}
	}

	Status = XST_SUCCESS;

done:
	return Status;
}

XStatus XPmClockPll_SetMode(XPm_PllClockNode *Pll, u32 Mode)
{
	XStatus Status = XST_FAILURE;
	u32 Val = 0;

	if ((u32)PM_PLL_MODE_FRACTIONAL == Mode) {
		/* Check if fractional value has been set */
		Status = XPmClockPll_GetParam(Pll, (u32)PM_PLL_PARAM_ID_DATA, &Val);
		if ((XST_SUCCESS != Status) || (0U == Val)) {
			Status = XST_FAILURE;
			goto done;
		}
	}

	Status = XPmClockPll_Reset(Pll, PLL_RESET_ASSERT);
	if (XST_SUCCESS != Status) {
		goto done;
	}

	if ((u32)PM_PLL_MODE_RESET == Mode) {
		Status = XST_SUCCESS;
		goto done;
	} else if ((u32)PM_PLL_MODE_FRACTIONAL == Mode) {
		/* Enable fractional mode */
		XPm_RMW32(Pll->FracConfigReg, PLL_FRAC_CFG_ENABLED_MASK,
			  PLL_FRAC_CFG_ENABLED_MASK);
	} else if ((u32)PM_PLL_MODE_INTEGER == Mode) {
		/* Disable fractional mode */
		XPm_RMW32(Pll->FracConfigReg, PLL_FRAC_CFG_ENABLED_MASK, 0);
	} else {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	Status = XPmClockPll_Reset(Pll, PLL_RESET_RELEASE);

done:
	if (XST_SUCCESS == Status) {
		Pll->PllMode = (u8)Mode;
	}
	return Status;
}

XStatus XPmClockPll_GetMode(XPm_PllClockNode *Pll, u32 *Mode)
{
	u32 Val;
	XStatus Status = XST_FAILURE;
	const XPm_Power *PowerDomain = Pll->ClkNode.PwrDomain;

	if ((u8)XPM_POWER_STATE_ON != PowerDomain->Node.State) {
		Status = XST_NO_ACCESS;
		goto done;
	}

	Val = XPm_Read32(Pll->ClkNode.Node.BaseAddress);
	if (0U != (Val & BIT32(Pll->Topology->ResetShift))) {
		*Mode = (u32)PM_PLL_MODE_RESET;
	} else {
		Val = XPm_Read32(Pll->FracConfigReg);
		if (0U != (Val & PLL_FRAC_CFG_ENABLED_MASK)) {
			*Mode = (u32)PM_PLL_MODE_FRACTIONAL;
		} else {
			*Mode = (u32)PM_PLL_MODE_INTEGER;
		}
	}

	Pll->PllMode = (u8)(*Mode);

	Status = XST_SUCCESS;

done:
	return Status;
}

XStatus XPmClockPll_SetParam(const XPm_PllClockNode *Pll, u32 Param, u32 Value)
{
	XStatus Status = XST_FAILURE;
	const XPm_PllParam *PtrParam;
	u32 Mask, ParamValue, Reg = 0;

	if (Param >= (u32)PM_PLL_PARAM_MAX) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	PtrParam = &Pll->Topology->ConfigParams[Param];
	if (Value > BITMASK(PtrParam->Width)) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	/* Allow config change only if PLL is in reset mode */
	if (Pll->ClkNode.Node.State != PM_PLL_STATE_RESET) {
		/*
		 * TODO - revisit it to allow subsystem CDO
		 * re-parsing which re-sets PLL parameters.
		 */
		PmInfo("Warning: Setting PLL parameter while not in reset state.\r\n");
		Status = XST_SUCCESS;
		goto done;
	}

	Mask = BITNMASK(PtrParam->Shift,PtrParam->Width);
	ParamValue = Value << PtrParam->Shift;

	switch (Param) {
	case (u32)PM_PLL_PARAM_ID_DIV2:
	case (u32)PM_PLL_PARAM_ID_FBDIV:
		Reg = Pll->ClkNode.Node.BaseAddress;
		Status = XST_SUCCESS;
		break;
	case (u32)PM_PLL_PARAM_ID_DATA:
		Reg = Pll->FracConfigReg;
		Status = XST_SUCCESS;
		break;
	case (u32)PM_PLL_PARAM_ID_PRE_SRC:
	case (u32)PM_PLL_PARAM_ID_POST_SRC:
	case (u32)PM_PLL_PARAM_ID_LOCK_DLY:
	case (u32)PM_PLL_PARAM_ID_LOCK_CNT:
	case (u32)PM_PLL_PARAM_ID_LFHF:
	case (u32)PM_PLL_PARAM_ID_CP:
	case (u32)PM_PLL_PARAM_ID_RES:
		Reg = Pll->ConfigReg;
		Status = XST_SUCCESS;
		break;
	default:
		Status = XST_FAILURE;
		break;
	}

	if (XST_SUCCESS == Status) {
		XPm_RMW32(Reg, Mask, ParamValue);
	}

done:
	return Status;
}

XStatus XPmClockPll_GetParam(const XPm_PllClockNode *Pll, u32 Param, u32 *Val)
{
	XStatus Status = XST_FAILURE;
	const XPm_PllParam *PtrParam;
	u32 Shift, Mask, Reg = 0;
	const XPm_Power *PowerDomain = Pll->ClkNode.PwrDomain;

	if ((u32)XPM_POWER_STATE_ON != PowerDomain->Node.State) {
		Status = XST_NO_ACCESS;
		goto done;
	}

	if (Param >= (u32)PM_PLL_PARAM_MAX) {
		Status = XST_INVALID_PARAM;
		goto done;
	}

	PtrParam = &Pll->Topology->ConfigParams[Param];
	Mask = BITNMASK(PtrParam->Shift, PtrParam->Width);
	Shift = PtrParam->Shift;

	switch (Param) {
	case (u32)PM_PLL_PARAM_ID_DIV2:
	case (u32)PM_PLL_PARAM_ID_FBDIV:
		Reg = Pll->ClkNode.Node.BaseAddress;
		Status = XST_SUCCESS;
		break;
	case (u32)PM_PLL_PARAM_ID_DATA:
		Reg = Pll->FracConfigReg;
		Status = XST_SUCCESS;
		break;
	case (u32)PM_PLL_PARAM_ID_PRE_SRC:
	case (u32)PM_PLL_PARAM_ID_POST_SRC:
	case (u32)PM_PLL_PARAM_ID_LOCK_DLY:
	case (u32)PM_PLL_PARAM_ID_LOCK_CNT:
	case (u32)PM_PLL_PARAM_ID_LFHF:
	case (u32)PM_PLL_PARAM_ID_CP:
	case (u32)PM_PLL_PARAM_ID_RES:
		Reg = Pll->ConfigReg;
		Status = XST_SUCCESS;
		break;
	default:
		Status = XST_FAILURE;
		break;
	}

	if (XST_SUCCESS == Status) {
		*Val = (XPm_Read32(Reg) & Mask) >> Shift;
	}

done:
	return Status;
}
