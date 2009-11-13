/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2009 by Øyvind Harboe                                   *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arm720t.h"
#include "time_support.h"
#include "target_type.h"


/*
 * ARM720 is an ARM7TDMI-S with MMU and ETM7.  For information, see
 * ARM DDI 0229C especially Chapter 9 about debug support.
 */

#if 0
#define _DEBUG_INSTRUCTION_EXECUTION_
#endif

static int arm720t_scan_cp15(target_t *target,
		uint32_t out, uint32_t *in, int instruction, int clock)
{
	int retval;
	struct arm720t_common *arm720t = target_to_arm720(target);
	arm_jtag_t *jtag_info;
	struct scan_field fields[2];
	uint8_t out_buf[4];
	uint8_t instruction_buf = instruction;

	jtag_info = &arm720t->arm7tdmi_common.arm7_9_common.jtag_info;

	buf_set_u32(out_buf, 0, 32, flip_u32(out, 32));

	jtag_set_end_state(TAP_DRPAUSE);
	if ((retval = arm_jtag_scann(jtag_info, 0xf)) != ERROR_OK)
	{
		return retval;
	}
	if ((retval = arm_jtag_set_instr(jtag_info, jtag_info->intest_instr, NULL)) != ERROR_OK)
	{
		return retval;
	}

	fields[0].tap = jtag_info->tap;
	fields[0].num_bits = 1;
	fields[0].out_value = &instruction_buf;
	fields[0].in_value = NULL;

	fields[1].tap = jtag_info->tap;
	fields[1].num_bits = 32;
	fields[1].out_value = out_buf;
	fields[1].in_value = NULL;

	if (in)
	{
		fields[1].in_value = (uint8_t *)in;
		jtag_add_dr_scan(2, fields, jtag_get_end_state());
		jtag_add_callback(arm7flip32, (jtag_callback_data_t)in);
	} else
	{
		jtag_add_dr_scan(2, fields, jtag_get_end_state());
	}

	if (clock)
		jtag_add_runtest(0, jtag_get_end_state());

#ifdef _DEBUG_INSTRUCTION_EXECUTION_
	if ((retval = jtag_execute_queue()) != ERROR_OK)
	{
		return retval;
	}

	if (in)
		LOG_DEBUG("out: %8.8x, in: %8.8x, instruction: %i, clock: %i", out, *in, instruction, clock);
	else
		LOG_DEBUG("out: %8.8x, instruction: %i, clock: %i", out, instruction, clock);
#else
		LOG_DEBUG("out: %8.8" PRIx32 ", instruction: %i, clock: %i", out, instruction, clock);
#endif

	return ERROR_OK;
}

static int arm720t_read_cp15(target_t *target, uint32_t opcode, uint32_t *value)
{
	/* fetch CP15 opcode */
	arm720t_scan_cp15(target, opcode, NULL, 1, 1);
	/* "DECODE" stage */
	arm720t_scan_cp15(target, ARMV4_5_NOP, NULL, 1, 1);
	/* "EXECUTE" stage (1) */
	arm720t_scan_cp15(target, ARMV4_5_NOP, NULL, 1, 0);
	arm720t_scan_cp15(target, 0x0, NULL, 0, 1);
	/* "EXECUTE" stage (2) */
	arm720t_scan_cp15(target, 0x0, NULL, 0, 1);
	/* "EXECUTE" stage (3), CDATA is read */
	arm720t_scan_cp15(target, ARMV4_5_NOP, value, 1, 1);

	return ERROR_OK;
}

static int arm720t_write_cp15(target_t *target, uint32_t opcode, uint32_t value)
{
	/* fetch CP15 opcode */
	arm720t_scan_cp15(target, opcode, NULL, 1, 1);
	/* "DECODE" stage */
	arm720t_scan_cp15(target, ARMV4_5_NOP, NULL, 1, 1);
	/* "EXECUTE" stage (1) */
	arm720t_scan_cp15(target, ARMV4_5_NOP, NULL, 1, 0);
	arm720t_scan_cp15(target, 0x0, NULL, 0, 1);
	/* "EXECUTE" stage (2) */
	arm720t_scan_cp15(target, value, NULL, 0, 1);
	arm720t_scan_cp15(target, ARMV4_5_NOP, NULL, 1, 1);

	return ERROR_OK;
}

static uint32_t arm720t_get_ttb(target_t *target)
{
	uint32_t ttb = 0x0;

	arm720t_read_cp15(target, 0xee120f10, &ttb);
	jtag_execute_queue();

	ttb &= 0xffffc000;

	return ttb;
}

static void arm720t_disable_mmu_caches(target_t *target,
		int mmu, int d_u_cache, int i_cache)
{
	uint32_t cp15_control;

	/* read cp15 control register */
	arm720t_read_cp15(target, 0xee110f10, &cp15_control);
	jtag_execute_queue();

	if (mmu)
		cp15_control &= ~0x1U;

	if (d_u_cache || i_cache)
		cp15_control &= ~0x4U;

	arm720t_write_cp15(target, 0xee010f10, cp15_control);
}

static void arm720t_enable_mmu_caches(target_t *target,
		int mmu, int d_u_cache, int i_cache)
{
	uint32_t cp15_control;

	/* read cp15 control register */
	arm720t_read_cp15(target, 0xee110f10, &cp15_control);
	jtag_execute_queue();

	if (mmu)
		cp15_control |= 0x1U;

	if (d_u_cache || i_cache)
		cp15_control |= 0x4U;

	arm720t_write_cp15(target, 0xee010f10, cp15_control);
}

static void arm720t_post_debug_entry(target_t *target)
{
	struct arm720t_common *arm720t = target_to_arm720(target);

	/* examine cp15 control reg */
	arm720t_read_cp15(target, 0xee110f10, &arm720t->cp15_control_reg);
	jtag_execute_queue();
	LOG_DEBUG("cp15_control_reg: %8.8" PRIx32 "", arm720t->cp15_control_reg);

	arm720t->armv4_5_mmu.mmu_enabled = (arm720t->cp15_control_reg & 0x1U) ? 1 : 0;
	arm720t->armv4_5_mmu.armv4_5_cache.d_u_cache_enabled = (arm720t->cp15_control_reg & 0x4U) ? 1 : 0;
	arm720t->armv4_5_mmu.armv4_5_cache.i_cache_enabled = 0;

	/* save i/d fault status and address register */
	arm720t_read_cp15(target, 0xee150f10, &arm720t->fsr_reg);
	arm720t_read_cp15(target, 0xee160f10, &arm720t->far_reg);
	jtag_execute_queue();
}

static void arm720t_pre_restore_context(target_t *target)
{
	struct arm720t_common *arm720t = target_to_arm720(target);

	/* restore i/d fault status and address register */
	arm720t_write_cp15(target, 0xee050f10, arm720t->fsr_reg);
	arm720t_write_cp15(target, 0xee060f10, arm720t->far_reg);
}

static int arm720t_verify_pointer(struct command_context_s *cmd_ctx,
		struct arm720t_common *arm720t)
{
	if (arm720t->common_magic != ARM720T_COMMON_MAGIC) {
		command_print(cmd_ctx, "target is not an ARM720");
		return ERROR_TARGET_INVALID;
	}
	return ERROR_OK;
}

static int arm720t_arch_state(struct target_s *target)
{
	struct arm720t_common *arm720t = target_to_arm720(target);
	struct armv4_5_common_s *armv4_5;

	static const char *state[] =
	{
		"disabled", "enabled"
	};

	armv4_5 = &arm720t->arm7tdmi_common.arm7_9_common.armv4_5_common;

	LOG_USER("target halted in %s state due to %s, current mode: %s\n"
			"cpsr: 0x%8.8" PRIx32 " pc: 0x%8.8" PRIx32 "\n"
			"MMU: %s, Cache: %s",
			 armv4_5_state_strings[armv4_5->core_state],
			 Jim_Nvp_value2name_simple(nvp_target_debug_reason, target->debug_reason)->name ,
			 armv4_5_mode_strings[armv4_5_mode_to_number(armv4_5->core_mode)],
			 buf_get_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 32),
			 buf_get_u32(armv4_5->core_cache->reg_list[15].value, 0, 32),
			 state[arm720t->armv4_5_mmu.mmu_enabled],
			 state[arm720t->armv4_5_mmu.armv4_5_cache.d_u_cache_enabled]);

	return ERROR_OK;
}

static int arm720_mmu(struct target_s *target, int *enabled)
{
	if (target->state != TARGET_HALTED) {
		LOG_ERROR("%s: target not halted", __func__);
		return ERROR_TARGET_INVALID;
	}

	*enabled = target_to_arm720(target)->armv4_5_mmu.mmu_enabled;
	return ERROR_OK;
}

static int arm720_virt2phys(struct target_s *target,
		uint32_t virt, uint32_t *phys)
{
	/** @todo Implement this!  */
	LOG_ERROR("%s: not implemented", __func__);
	return ERROR_FAIL;
}

static int arm720t_read_memory(struct target_s *target,
		uint32_t address, uint32_t size, uint32_t count, uint8_t *buffer)
{
	int retval;
	struct arm720t_common *arm720t = target_to_arm720(target);

	/* disable cache, but leave MMU enabled */
	if (arm720t->armv4_5_mmu.armv4_5_cache.d_u_cache_enabled)
		arm720t_disable_mmu_caches(target, 0, 1, 0);

	retval = arm7_9_read_memory(target, address, size, count, buffer);

	if (arm720t->armv4_5_mmu.armv4_5_cache.d_u_cache_enabled)
		arm720t_enable_mmu_caches(target, 0, 1, 0);

	return retval;
}

static int arm720t_read_phys_memory(struct target_s *target,
		uint32_t address, uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct arm720t_common *arm720t = target_to_arm720(target);

	return armv4_5_mmu_read_physical(target, &arm720t->armv4_5_mmu, address, size, count, buffer);
}

static int arm720t_write_phys_memory(struct target_s *target,
		uint32_t address, uint32_t size, uint32_t count, uint8_t *buffer)
{
	struct arm720t_common *arm720t = target_to_arm720(target);

	return armv4_5_mmu_write_physical(target, &arm720t->armv4_5_mmu, address, size, count, buffer);
}

static int arm720t_soft_reset_halt(struct target_s *target)
{
	int retval = ERROR_OK;
	struct arm720t_common *arm720t = target_to_arm720(target);
	reg_t *dbg_stat = &arm720t->arm7tdmi_common.arm7_9_common
			.eice_cache->reg_list[EICE_DBG_STAT];
	struct armv4_5_common_s *armv4_5 = &arm720t->arm7tdmi_common
			.arm7_9_common.armv4_5_common;

	if ((retval = target_halt(target)) != ERROR_OK)
	{
		return retval;
	}

	long long then = timeval_ms();
	int timeout;
	while (!(timeout = ((timeval_ms()-then) > 1000)))
	{
		if (buf_get_u32(dbg_stat->value, EICE_DBG_STATUS_DBGACK, 1) == 0)
		{
			embeddedice_read_reg(dbg_stat);
			if ((retval = jtag_execute_queue()) != ERROR_OK)
			{
				return retval;
			}
		} else
		{
			break;
		}
		if (debug_level >= 3)
		{
			alive_sleep(100);
		} else
		{
			keep_alive();
		}
	}
	if (timeout)
	{
		LOG_ERROR("Failed to halt CPU after 1 sec");
		return ERROR_TARGET_TIMEOUT;
	}

	target->state = TARGET_HALTED;

	/* SVC, ARM state, IRQ and FIQ disabled */
	buf_set_u32(armv4_5->core_cache->reg_list[ARMV4_5_CPSR].value, 0, 8, 0xd3);
	armv4_5->core_cache->reg_list[ARMV4_5_CPSR].dirty = 1;
	armv4_5->core_cache->reg_list[ARMV4_5_CPSR].valid = 1;

	/* start fetching from 0x0 */
	buf_set_u32(armv4_5->core_cache->reg_list[15].value, 0, 32, 0x0);
	armv4_5->core_cache->reg_list[15].dirty = 1;
	armv4_5->core_cache->reg_list[15].valid = 1;

	armv4_5->core_mode = ARMV4_5_MODE_SVC;
	armv4_5->core_state = ARMV4_5_STATE_ARM;

	arm720t_disable_mmu_caches(target, 1, 1, 1);
	arm720t->armv4_5_mmu.mmu_enabled = 0;
	arm720t->armv4_5_mmu.armv4_5_cache.d_u_cache_enabled = 0;
	arm720t->armv4_5_mmu.armv4_5_cache.i_cache_enabled = 0;

	if ((retval = target_call_event_callbacks(target, TARGET_EVENT_HALTED)) != ERROR_OK)
	{
		return retval;
	}

	return ERROR_OK;
}

static int arm720t_init_target(struct command_context_s *cmd_ctx, struct target_s *target)
{
	return arm7tdmi_init_target(cmd_ctx, target);
}

static int arm720t_init_arch_info(target_t *target,
		struct arm720t_common *arm720t, struct jtag_tap *tap)
{
	arm7tdmi_common_t *arm7tdmi = &arm720t->arm7tdmi_common;
	arm7_9_common_t *arm7_9 = &arm7tdmi->arm7_9_common;

	arm7tdmi_init_arch_info(target, arm7tdmi, tap);

	arm720t->common_magic = ARM720T_COMMON_MAGIC;

	arm7_9->post_debug_entry = arm720t_post_debug_entry;
	arm7_9->pre_restore_context = arm720t_pre_restore_context;

	arm720t->armv4_5_mmu.armv4_5_cache.ctype = -1;
	arm720t->armv4_5_mmu.get_ttb = arm720t_get_ttb;
	arm720t->armv4_5_mmu.read_memory = arm7_9_read_memory;
	arm720t->armv4_5_mmu.write_memory = arm7_9_write_memory;
	arm720t->armv4_5_mmu.disable_mmu_caches = arm720t_disable_mmu_caches;
	arm720t->armv4_5_mmu.enable_mmu_caches = arm720t_enable_mmu_caches;
	arm720t->armv4_5_mmu.has_tiny_pages = 0;
	arm720t->armv4_5_mmu.mmu_enabled = 0;

	return ERROR_OK;
}

static int arm720t_target_create(struct target_s *target, Jim_Interp *interp)
{
	struct arm720t_common *arm720t = calloc(1, sizeof(*arm720t));

	arm720t->arm7tdmi_common.arm7_9_common.armv4_5_common.is_armv4 = true;
	return arm720t_init_arch_info(target, arm720t, target->tap);
}

COMMAND_HANDLER(arm720t_handle_cp15_command)
{
	int retval;
	target_t *target = get_current_target(cmd_ctx);
	struct arm720t_common *arm720t = target_to_arm720(target);
	arm_jtag_t *jtag_info;

	retval = arm720t_verify_pointer(cmd_ctx, arm720t);
	if (retval != ERROR_OK)
		return retval;

	jtag_info = &arm720t->arm7tdmi_common.arm7_9_common.jtag_info;

	if (target->state != TARGET_HALTED)
	{
		command_print(cmd_ctx, "target must be stopped for \"%s\" command", CMD_NAME);
		return ERROR_OK;
	}

	/* one or more argument, access a single register (write if second argument is given */
	if (argc >= 1)
	{
		uint32_t opcode;
		COMMAND_PARSE_NUMBER(u32, args[0], opcode);

		if (argc == 1)
		{
			uint32_t value;
			if ((retval = arm720t_read_cp15(target, opcode, &value)) != ERROR_OK)
			{
				command_print(cmd_ctx, "couldn't access cp15 with opcode 0x%8.8" PRIx32 "", opcode);
				return ERROR_OK;
			}

			if ((retval = jtag_execute_queue()) != ERROR_OK)
			{
				return retval;
			}

			command_print(cmd_ctx, "0x%8.8" PRIx32 ": 0x%8.8" PRIx32 "", opcode, value);
		}
		else if (argc == 2)
		{
			uint32_t value;
			COMMAND_PARSE_NUMBER(u32, args[1], value);

			if ((retval = arm720t_write_cp15(target, opcode, value)) != ERROR_OK)
			{
				command_print(cmd_ctx, "couldn't access cp15 with opcode 0x%8.8" PRIx32 "", opcode);
				return ERROR_OK;
			}
			command_print(cmd_ctx, "0x%8.8" PRIx32 ": 0x%8.8" PRIx32 "", opcode, value);
		}
	}

	return ERROR_OK;
}

static int arm720t_mrc(target_t *target, int cpnum, uint32_t op1, uint32_t op2, uint32_t CRn, uint32_t CRm, uint32_t *value)
{
	if (cpnum!=15)
	{
		LOG_ERROR("Only cp15 is supported");
		return ERROR_FAIL;
	}

	return arm720t_read_cp15(target, mrc_opcode(cpnum, op1, op2, CRn, CRm), value);

}

static int arm720t_mcr(target_t *target, int cpnum, uint32_t op1, uint32_t op2, uint32_t CRn, uint32_t CRm, uint32_t value)
{
	if (cpnum!=15)
	{
		LOG_ERROR("Only cp15 is supported");
		return ERROR_FAIL;
	}

	return arm720t_write_cp15(target, mrc_opcode(cpnum, op1, op2, CRn, CRm), value);
}

static int arm720t_register_commands(struct command_context_s *cmd_ctx)
{
	int retval;
	command_t *arm720t_cmd;


	retval = arm7_9_register_commands(cmd_ctx);

	arm720t_cmd = register_command(cmd_ctx, NULL, "arm720t",
			NULL, COMMAND_ANY,
			"arm720t specific commands");

	register_command(cmd_ctx, arm720t_cmd, "cp15",
			arm720t_handle_cp15_command, COMMAND_EXEC,
			"display/modify cp15 register <opcode> [value]");

	return ERROR_OK;
}

/** Holds methods for ARM720 targets. */
target_type_t arm720t_target =
{
	.name = "arm720t",

	.poll = arm7_9_poll,
	.arch_state = arm720t_arch_state,

	.halt = arm7_9_halt,
	.resume = arm7_9_resume,
	.step = arm7_9_step,

	.assert_reset = arm7_9_assert_reset,
	.deassert_reset = arm7_9_deassert_reset,
	.soft_reset_halt = arm720t_soft_reset_halt,

	.get_gdb_reg_list = armv4_5_get_gdb_reg_list,

	.read_memory = arm720t_read_memory,
	.write_memory = arm7_9_write_memory,
	.read_phys_memory = arm720t_read_phys_memory,
	.write_phys_memory = arm720t_write_phys_memory,
	.mmu = arm720_mmu,
	.virt2phys = arm720_virt2phys,

	.bulk_write_memory = arm7_9_bulk_write_memory,
	.checksum_memory = arm7_9_checksum_memory,
	.blank_check_memory = arm7_9_blank_check_memory,

	.run_algorithm = armv4_5_run_algorithm,

	.add_breakpoint = arm7_9_add_breakpoint,
	.remove_breakpoint = arm7_9_remove_breakpoint,
	.add_watchpoint = arm7_9_add_watchpoint,
	.remove_watchpoint = arm7_9_remove_watchpoint,

	.register_commands = arm720t_register_commands,
	.target_create = arm720t_target_create,
	.init_target = arm720t_init_target,
	.examine = arm7tdmi_examine,
	.mrc = arm720t_mrc,
	.mcr = arm720t_mcr,

};
