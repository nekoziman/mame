// license:BSD-3-Clause
// copyright-holders:AJR
/***************************************************************************

    DEC RX01 skeleton CPU device

***************************************************************************/

#include "emu.h"
#include "rx01.h"
#include "rx01dasm.h"

//#define VERBOSE 1
#include "logmacro.h"

// device type definition
DEFINE_DEVICE_TYPE(RX01_CPU, rx01_cpu_device, "rx01_cpu", "DEC RX01 CPU")

rx01_cpu_device::rx01_cpu_device(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
	: cpu_device(mconfig, RX01_CPU, tag, owner, clock)
	, m_inst_config("program", ENDIANNESS_LITTLE, 8, 12, 0)
	, m_sp_config("scratchpad", ENDIANNESS_LITTLE, 8, 4, 0, address_map_constructor(FUNC(rx01_cpu_device::scratchpad_map), this))
	, m_inst_cache(nullptr)
	, m_sp_cache(nullptr)
	, m_pc(0)
	, m_ppc(0)
	, m_mb(0)
	, m_br_condition(false)
	, m_inst_disable(false)
	, m_inst_repeat(false)
	, m_cntr(0)
	, m_sr(0)
	, m_spar(0)
	, m_bar(0)
	, m_crc(0)
	, m_flag(false)
	, m_icount(0)
{
	m_inst_config.m_is_octal = true;
	m_sp_config.m_is_octal = true;
}

std::unique_ptr<util::disasm_interface> rx01_cpu_device::create_disassembler()
{
	return std::make_unique<rx01_disassembler>();
}

void rx01_cpu_device::scratchpad_map(address_map &map)
{
	map(0, 15).ram().share("scratchpad"); // two 7489 16x4 register files
}

device_memory_interface::space_config_vector rx01_cpu_device::memory_space_config() const
{
	return space_config_vector {
		std::make_pair(AS_PROGRAM, &m_inst_config),
		std::make_pair(AS_DATA, &m_sp_config)
	};
}

void rx01_cpu_device::device_start()
{
	m_inst_cache = space(AS_PROGRAM).cache<0, 0, ENDIANNESS_LITTLE>();
	m_sp_cache = space(AS_DATA).cache<0, 0, ENDIANNESS_LITTLE>();

	set_icountptr(m_icount);

	// Debug state registration
	state_add(RX01_PC, "PC", m_pc).mask(07777).formatstr("%04O");
	state_add(STATE_GENPC, "GENPC", m_pc).mask(07777).formatstr("%04O").noshow();
	state_add(STATE_GENPCBASE, "CURPC", m_pc).mask(07777).formatstr("%04O").noshow();
	state_add(RX01_CNTR, "CNTR", m_cntr).formatstr("%03O");
	state_add(RX01_SR, "SR", m_sr).formatstr("%03O");
	state_add(RX01_SPAR, "SPAR", m_spar).mask(15).formatstr("%3s");
	u8 *sp = static_cast<u8 *>(memshare("scratchpad")->ptr());
	for (int r = 0; r < 16; r++)
		state_add(RX01_R0 + r, string_format("R%d", r).c_str(), sp[r]).formatstr("%03O");
	state_add(RX01_BAR, "BAR", m_bar).mask(07777).formatstr("%04O");
	state_add(RX01_CRC, "CRC", m_crc).formatstr("%06O");

	// Save state registration
	save_item(NAME(m_pc));
	save_item(NAME(m_ppc));
	save_item(NAME(m_mb));
	save_item(NAME(m_br_condition));
	save_item(NAME(m_inst_disable));
	save_item(NAME(m_inst_repeat));
	save_item(NAME(m_cntr));
	save_item(NAME(m_sr));
	save_item(NAME(m_spar));
	save_item(NAME(m_bar));
	save_item(NAME(m_crc));
	save_item(NAME(m_flag));
}

void rx01_cpu_device::device_reset()
{
	// Clear address registers, counters and flags
	m_pc = 0;
	m_mb = 0;
	m_inst_disable = false;
	m_inst_repeat = false;
	m_bar = 0;
	m_cntr = 0;
	m_sr = 0;
	m_spar = 0;
	m_flag = false;
}

u8 rx01_cpu_device::mux_out()
{
	return BIT(m_mb, 0) ? m_sp_cache->read_byte(m_spar) : m_inst_cache->read_byte(m_pc);
}

bool rx01_cpu_device::test_condition()
{
	switch (m_mb & 074)
	{
	case 020:
		return BIT(m_sr, 7);

	case 024:
		return m_cntr == 0377;

	case 030:
		return BIT(m_crc, 0);

	case 074:
		return m_flag;

	default:
		LOG("%04o: Unhandled branch condition %d\n", m_ppc, (m_mb & 074) >> 2);
		return true;
	}
}

void rx01_cpu_device::execute_run()
{
	while (m_icount > 0)
	{
		if (m_inst_disable)
		{
			if ((m_mb & 0302) == 0202)
				m_pc = u16(m_mb & 074) << 6 | mux_out();
			else if (BIT(m_mb, 6) && m_br_condition)
				m_pc = ((m_pc + 1) & 07400) | mux_out();
			else
				m_pc = (m_pc + 1) & 07777;

			m_inst_disable = false;
			m_inst_repeat = false;
		}
		else
		{
			if (!m_inst_repeat)
			{
				m_ppc = m_pc;
				debugger_instruction_hook(m_pc);

				m_mb = m_inst_cache->read_byte(m_pc);
				m_pc = (m_pc + 1) & 03777;
			}

			if (BIT(m_mb, 6))
			{
				m_br_condition = test_condition() == BIT(m_mb, 1);
				if (BIT(m_mb, 7))
				{
					m_inst_disable = m_cntr == 0377 || m_br_condition;
					m_inst_repeat = m_cntr != 0377 && !m_br_condition;
					m_cntr++;
				}
				else
					m_inst_disable = true;
			}
			else if (BIT(m_mb, 7))
			{
				if (BIT(m_mb, 1))
					m_inst_disable = true;
				else
					m_spar = (m_mb & 074) >> 2;
			}
			else switch (m_mb & 074)
			{
			case 044:
				if (BIT(m_mb, 1))
					m_bar = (m_bar + 1) & 07777;
				else
					m_bar = BIT(m_mb, 0) ? 0 : 06000;
				break;

			case 060:
				m_flag = (!BIT(m_mb, 0) && m_flag) || (BIT(m_mb, 1) && !m_flag);
				break;

			case 064:
				m_sp_cache->write_byte(m_spar, m_sr);
				break;

			case 070:
				if (BIT(m_mb, 1))
					m_cntr++;
				else
					m_cntr = mux_out();
				m_inst_disable = !BIT(m_mb, 0);
				break;

			case 074:
				if ((m_mb & 3) == 1)
					m_sr = m_cntr;
				else
					m_sr = (m_sr << 1) | (BIT(m_mb, 0) ? 0 /*sep_data()*/ : BIT(m_mb, 1));
				break;

			default:
				LOG("%04o: Unimplemented instruction %03o\n", m_ppc, m_mb);
				break;
			}
		}

		m_icount--;
	}
}

void rx01_cpu_device::state_string_export(const device_state_entry &entry, std::string &str) const
{
	switch (entry.index())
	{
	case RX01_SPAR:
		str = string_format("R%-2d", m_spar);
		break;
	}
}
