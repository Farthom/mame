// license:BSD-3-Clause
// copyright-holders: R. Belmont, O. Galibert
/****************************************************************************

    drivers/mac128.cpp
    Original-style Macintosh emulation

    These are all 68000 machines in the original Mac form factor with the
    original Mac audio and video.

    Unitron Mac 512: Brazilian Mac 512K(E?) clone.
    Unitron 1024: Brazilian Mac Plus clone.

    Driver by R. Belmont and O. Galibert, with thanks to the original Mac
    driver authors Nathan Woods and Raphael Nabet.
    Thanks also to SCSI guru Patrick Mackinlay and keyboard/mouse wrangler
    Vas Crabb.

    Mac 128K/512K: the original machines with 128K or 512K of RAM.
    Mac Plus: floppy now double-sided 800K, SIMM slots for memory expansion,
              SCSI interface added.
    Mac 512KE: a Mac 512K with the 800K floppy drive and the newer Mac Plus ROMs.
    Mac SE: Mac Plus with ADB for the keyboard and mouse interface, and an
            expansion slot.
    Mac SE FDHD: Mac SE with the IWM and 800K drive upgraded to SWIM and
                 the 1.44MB "SuperDrive".
    Mac Classic: Cost-reduced Mac SE FDHD.

    Memory Map:
    0x000000 - 0x3fffff     RAM/ROM (switches based on overlay)
    0x400000 - 0x4fffff     ROM
    0x580000 - 0x5fffff     5380 NCR/Symbios SCSI peripherals chip (Mac Plus only)
    0x600000 - 0x6fffff     RAM
    0x800000 - 0x9fffff     Zilog 8530 SCC (Serial Communications Controller) Read
    0xa00000 - 0xbfffff     Zilog 8530 SCC (Serial Communications Controller) Write
    0xc00000 - 0xdfffff     IWM (Integrated Woz Machine) floppy controller
    0xe80000 - 0xefffff     Rockwell 6522 VIA
    0xf00000 - 0xffffef     Open bus??? (the ROM appears to be accessing here)
    0xfffff0 - 0xffffff     Auto Vector

    Interrupts:
        M68K:
            Level 1 from VIA
            Level 2 from SCC
            Level 4 from "programmer switch" (not implemented)
        VIA:
            CA1 from VBLANK
            CA2 from 1 Hz clock (RTC)
            CB1 from Keyboard Clock
            CB2 from Keyboard Data
            SR  from Keyboard Data Ready

        SCC:
            PB_EXT (DCDB)  from mouse Y circuitry
            PA_EXT (DCDA)  from mouse X circuitry

    The MC68000's FC outputs are not used even for autovectoring. The
    VIA's address range is overdecoded to generate VPA for both.

VIA notes:

Original 128K Macs use Synertek SYP6522 VIAs. Synertek's AN5 (March 1982)
claims that their VIAs have a difference in shift register behavior
compared to those from other manufacturers. However, this particular
difference is not relevant to the Mac hardware, and later 512K Macs
switched away from SYP6522 (Apple part number 338-6522-A) to either
Rockwell R6522AP (R6522-66) or VTI VL6522-02PC. R6522AP is the most
common VIA on the Mac Plus.

Apple continued sourcing VIAs from the same two manufacturers for the
Mac SE, though their ADB-era VIAs are newer CMOS models with a fix for the
shift register's CB1 input synchronizer which Apple may have specifically
requested. The new Rockwell version (R65NC22) is only labeled with Apple
part number 338-6523 (later Macs use a PLCC version which Apple numbered
338S6523), but VLSI Technology's VL65C22V-02PC is not so disguised.

****************************************************************************/

#include "emu.h"

#include "machine/macrtc.h"

#include "bus/mackbd/mackbd.h"
#include "bus/macpds/hyperdrive.h"
#include "bus/scsi/scsicd.h"
#include "bus/scsi/scsi.h"
#include "bus/scsi/scsihd.h"
#include "cpu/m68000/m68000.h"
#include "machine/6522via.h"
#include "machine/iwm.h"
#include "machine/swim1.h"
#include "machine/ncr5380n.h"
#include "machine/nscsi_bus.h"
#include "bus/nscsi/hd.h"
#include "bus/nscsi/cd.h"
#include "machine/ncr5380.h"
#include "machine/ram.h"
#include "machine/applefdintf.h"
#include "machine/timer.h"
#include "machine/z80scc.h"
#include "machine/macadb.h"
#include "sound/dac.h"
#include "bus/macpds/pds_tpdfpd.h"

#include "formats/ap_dsk35.h"

#include "emupal.h"
#include "screen.h"
#include "softlist.h"
#include "speaker.h"

#define C7M (15.6672_MHz_XTAL / 2)
#define C3_7M (15.6672_MHz_XTAL / 4).value()

// video parameters
static constexpr int MAC_H_VIS   = 512;
static constexpr int MAC_V_VIS   = 342;
static constexpr int MAC_H_TOTAL = 704;       // (512+192)
static constexpr int MAC_V_TOTAL = 370;       // (342+28)

// sound buffer locations
static constexpr int MAC_MAIN_SND_BUF_OFFSET = (0x0300>>1);
static constexpr int MAC_ALT_SND_BUF_OFFSET  = (0x5F00>>1);

class mac128_state : public driver_device
{
public:
	mac128_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_via(*this, "via6522_0"),
		m_macadb(*this, "macadb"),
		m_ram(*this, RAM_TAG),
		m_ncr5380(*this, "ncr5380"),
		m_scsibus(*this, "scsibus"),
		m_ncr5380n(*this, "scsibus:7:ncr5380n"),
		m_iwm(*this, "fdc"),
		m_floppy(*this, "fdc:%d", 0U),
		m_mackbd(*this, "kbd"),
		m_rtc(*this,"rtc"),
		m_screen(*this, "screen"),
		m_dac(*this, "macdac"),
		m_scc(*this, "scc"),
		m_mouse0(*this, "MOUSE0"),
		m_mouse1(*this, "MOUSE1"),
		m_mouse2(*this, "MOUSE2"),
		m_cur_floppy(nullptr),
		m_hdsel(0),
		m_devsel(0)
	{
	}

	void mac512ke(machine_config &config);
	void mac128k(machine_config &config);
	void mac512k(machine_config &config);
	void macplus(machine_config &config);
	void macse(machine_config &config);
	void macsefd(machine_config &config);
	void macclasc(machine_config &config);

	void mac_driver_init();

private:
	required_device<m68000_device> m_maincpu;
	required_device<via6522_device> m_via;
	optional_device<macadb_device> m_macadb;
	required_device<ram_device> m_ram;
	optional_device<ncr5380_device> m_ncr5380;
	optional_device<nscsi_bus_device> m_scsibus;
	optional_device<ncr5380n_device> m_ncr5380n;
	required_device<applefdintf_device> m_iwm;
	required_device_array<floppy_connector, 2> m_floppy;
	optional_device<mac_keyboard_port_device> m_mackbd;
	optional_device<rtc3430042_device> m_rtc;
	required_device<screen_device> m_screen;
	required_device<dac_8bit_pwm_device> m_dac;
	required_device<z80scc_device> m_scc;

	optional_ioport m_mouse0, m_mouse1, m_mouse2;

	virtual void machine_start() override;
	virtual void machine_reset() override;

	void scc_mouse_irq( int x, int y );
	void set_via_interrupt(int value);
	void field_interrupts();
	void vblank_irq();
	void mouse_callback();

	uint16_t ram_r(offs_t offset);
	void ram_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	void ram_w_se(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	uint16_t ram_600000_r(offs_t offset);
	void ram_600000_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~ 0);
	void via_sync();
	void via_sync_end();
	uint16_t mac_via_r(offs_t offset);
	void mac_via_w(offs_t offset, uint16_t data);
	uint16_t mac_autovector_r(offs_t offset);
	void mac_autovector_w(offs_t offset, uint16_t data);
	uint16_t mac_iwm_r(offs_t offset, uint16_t mem_mask = ~0);
	void mac_iwm_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	uint16_t macplus_scsi_r(offs_t offset, uint16_t mem_mask = ~0);
	void macplus_scsi_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	uint16_t macse_scsi_r(offs_t offset, uint16_t mem_mask = ~0);
	void macse_scsi_w(offs_t offset, uint16_t data, uint16_t mem_mask = ~0);
	DECLARE_WRITE_LINE_MEMBER(scsi_irq_w);
	DECLARE_WRITE_LINE_MEMBER(scsi_drq_w);
	DECLARE_WRITE_LINE_MEMBER(set_scc_interrupt);
	DECLARE_WRITE_LINE_MEMBER(vblank_w);

	WRITE_LINE_MEMBER(adb_irq_w) { m_adb_irq_pending = state; }

	TIMER_CALLBACK_MEMBER(mac_scanline);
	TIMER_CALLBACK_MEMBER(mac_hblank);
	uint32_t screen_update_mac(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect);

	uint8_t mac_via_in_a();
	uint8_t mac_via_in_b();
	uint8_t mac_via_in_b_se();
	void mac_via_out_a(uint8_t data);
	void mac_via_out_b(uint8_t data);
	void mac_via_out_a_se(uint8_t data);
	void mac_via_out_b_se(uint8_t data);
	DECLARE_WRITE_LINE_MEMBER(mac_via_irq);
	void update_volume();

	void mac512ke_map(address_map &map);
	void macplus_map(address_map &map);
	void macse_map(address_map &map);

	floppy_image_device *m_cur_floppy;
	int m_hdsel, m_devsel;
	int m_pwm_count_total, m_pwm_count_1;
	float m_pwm_current_rpm[2];

	void phases_w(uint8_t phases);
	void devsel_w(uint8_t devsel);
	void devsel_se_w(uint8_t devsel);
	void pwm_push(uint8_t data);

	uint32_t m_overlay;

	int m_irq_count, m_ca2_data;
	uint8_t m_mouse_bit[2], m_mouse_last[2];
	int16_t m_mouse_last_m[2], m_mouse_count[2];
	int m_screen_buffer;
	emu_timer *m_scan_timer;
	emu_timer *m_hblank_timer;

	// interrupts
	int m_scc_interrupt, m_via_interrupt, m_scsi_interrupt, m_last_taken_interrupt;

	// DRQ
	int m_scsi_drq;

	// wait states for accessing the VIA
	bool m_snd_enable;
	bool m_main_buffer;
	int m_snd_vol;
	int m_adb_irq_pending;
	int m_drive_select;
	int m_scsiirq_enable;
	u16 *m_ram_ptr, *m_rom_ptr;
	u32 m_ram_mask, m_ram_size;
};

void mac128_state::machine_start()
{
	m_ram_ptr = (u16*)m_ram->pointer();
	m_ram_size = m_ram->size()>>1;
	m_ram_mask = m_ram_size - 1;
	m_rom_ptr = (u16*)memregion("bootrom")->base();

	m_scan_timer = machine().scheduler().timer_alloc(timer_expired_delegate(FUNC(mac128_state::mac_scanline), this));
	m_hblank_timer = machine().scheduler().timer_alloc(timer_expired_delegate(FUNC(mac128_state::mac_hblank), this));

	save_item(NAME(m_overlay));
	save_item(NAME(m_irq_count));
	save_item(NAME(m_ca2_data));
	save_item(NAME(m_mouse_bit));
	save_item(NAME(m_mouse_last));
	save_item(NAME(m_mouse_last_m));
	save_item(NAME(m_mouse_count));
	save_item(NAME(m_screen_buffer));
	save_item(NAME(m_scc_interrupt));
	save_item(NAME(m_via_interrupt));
	save_item(NAME(m_scsi_interrupt));
	save_item(NAME(m_scsi_drq));
	save_item(NAME(m_last_taken_interrupt));
	save_item(NAME(m_snd_enable));
	save_item(NAME(m_main_buffer));
	save_item(NAME(m_snd_vol));
	save_item(NAME(m_adb_irq_pending));
	save_item(NAME(m_drive_select));
	save_item(NAME(m_scsiirq_enable));
	save_item(NAME(m_pwm_count_total));
	save_item(NAME(m_pwm_count_1));
	save_item(NAME(m_pwm_current_rpm));

	m_mouse_bit[0] = m_mouse_bit[1] = 0;
	m_mouse_last[0] = m_mouse_last[1] = 0;
}

void mac128_state::machine_reset()
{
	m_last_taken_interrupt = -1;
	m_overlay = 1;
	m_screen_buffer = 1;
	m_last_taken_interrupt = 0;
	m_snd_enable = false;
	m_main_buffer = true;
	m_snd_vol = 3;
	m_irq_count = 0;
	m_ca2_data = 0;
	m_adb_irq_pending = 0;
	m_drive_select = 0;
	m_scsiirq_enable = 0;
	m_pwm_count_total = 0;
	m_pwm_count_1 = 0;
	m_pwm_current_rpm[0] = 302.5; // Speed for 0% duty cycle
	m_pwm_current_rpm[1] = 302.5;
	m_scsi_drq = 0;

	const int next_vpos = m_screen->vpos() + 1;
	m_scan_timer->adjust(m_screen->time_until_pos(next_vpos), next_vpos);
	if (m_screen->vblank())
		m_via->write_pb6(0);
}

uint16_t mac128_state::ram_r(offs_t offset)
{
	if (m_overlay)
	{
		return m_rom_ptr[offset & 0x7ffff];
	}

	return m_ram_ptr[offset & m_ram_mask];
}

void mac128_state::ram_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	if (!m_overlay)
	{
		COMBINE_DATA(&m_ram_ptr[offset & m_ram_mask]);
	}
}

void mac128_state::ram_w_se(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	m_overlay = 0;
	COMBINE_DATA(&m_ram_ptr[offset & m_ram_mask]);
}

uint16_t mac128_state::ram_600000_r(offs_t offset)
{
	return m_ram_ptr[offset & m_ram_mask];
}

void mac128_state::ram_600000_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	COMBINE_DATA(&m_ram_ptr[offset & m_ram_mask]);
}

void mac128_state::field_interrupts()
{
	int take_interrupt = -1;

	if ((m_scc_interrupt) || (m_scsi_interrupt))
	{
		take_interrupt = 2;
	}
	else if (m_via_interrupt)
	{
		take_interrupt = 1;
	}

//  printf("field_interrupts: take %d\n", take_interrupt);

	if (m_last_taken_interrupt > -1)
	{
		m_maincpu->set_input_line(m_last_taken_interrupt, CLEAR_LINE);
		m_last_taken_interrupt = -1;
	}

	if (take_interrupt > -1)
	{
		m_maincpu->set_input_line(take_interrupt, ASSERT_LINE);
		m_last_taken_interrupt = take_interrupt;
	}
}

WRITE_LINE_MEMBER(mac128_state::set_scc_interrupt)
{
//  printf("SCC IRQ: %d\n", state);
	m_scc_interrupt = state;
	field_interrupts();
}

void mac128_state::set_via_interrupt(int value)
{
	m_via_interrupt = value;
	field_interrupts();
}

void mac128_state::vblank_irq()
{
	if (m_macadb)
	{
		m_macadb->adb_vblank();
	}

	if (++m_irq_count == 60)
	{
		m_irq_count = 0;

		m_ca2_data ^= 1;
		/* signal 1 Hz irq on CA2 input on the VIA */
		m_via->write_ca2(m_ca2_data);
	}
}

void mac128_state::update_volume()
{
	if (!m_snd_enable)
	{
		// ls161 clear input
		m_dac->set_output_gain(ALL_OUTPUTS, 0);
	}
	else
	{
		// sound -> r13 (470k)
		// sound -> r12 (470k) -> 4016 (pa0 != 0)
		// sound -> r17 (150k) -> 4016 (pa1 != 0)
		// sound -> r16 (68k)  -> 4016 (pa2 != 0)
		m_dac->set_output_gain(ALL_OUTPUTS, 8.0 / (m_snd_vol + 1));
	}
}

WRITE_LINE_MEMBER(mac128_state::vblank_w)
{
	m_via->write_ca1(state);
}

TIMER_CALLBACK_MEMBER(mac128_state::mac_scanline)
{
	int scanline = param;
	uint16_t *mac_snd_buf_ptr;

	if (scanline == MAC_V_VIS)
	{
		vblank_irq();
	}

	/* video beam in display (! VBLANK && ! HBLANK basically) */
	if (scanline < MAC_V_VIS)
	{
		m_via->write_pb6(1);
		m_hblank_timer->adjust(m_screen->time_until_pos(scanline, MAC_H_VIS));
	}

	if ((!(scanline % 10)) && (!m_macadb))
	{
		mouse_callback();
	}

	if (m_main_buffer)
	{
		mac_snd_buf_ptr = (uint16_t *)(m_ram_ptr + m_ram_size - MAC_MAIN_SND_BUF_OFFSET);
	}
	else
	{
		mac_snd_buf_ptr = (uint16_t *)(m_ram_ptr + m_ram_size - MAC_ALT_SND_BUF_OFFSET);
	}

	m_dac->write(mac_snd_buf_ptr[scanline] >> 8);
	pwm_push(mac_snd_buf_ptr[scanline] & 0xff);
	m_scan_timer->adjust(m_screen->time_until_pos(scanline+1), (scanline+1) % m_screen->height());
}

TIMER_CALLBACK_MEMBER(mac128_state::mac_hblank)
{
	m_via->write_pb6(0);
}

void mac128_state::pwm_push(uint8_t data)
{
	// The PWM works by sending pulses with a specific duty cycle.
	// The lengths sent by the firmware are in the range 1-40, which
	// means the total number of time slots is probably 42, to ensure
	// at least one edge always happens.  To get a better precision
	// the firmware dithers between two values over a cycle of 10
	// pulses, giving internally a 0-399 possible range mapping to a
	// 11-410 real length out of 420 total, with a duty cycle ranging
	// from 2.6% to 97.6%.  The firmware calibrates from the drive
	// actual rpm as measured through the tachometer with indexes 128
	// and 256 at startup and keeps an eye on the actual rpm
	// afterwards to avoid temperature drift.

	// The length counter is a 6-bits lfsr with taps on bits 0 and 1
	// and insertion on bit 5.  The firmware writes a value so that
	// the length is reached when the counter hits 0x20.

	static const uint8_t value_to_length[64] = {
		 0,  1, 59,  2, 60, 40, 54,  3,
		61, 32, 49, 41, 55, 19, 35,  4,
		62, 52, 30, 33, 50, 12, 14, 42,
		56, 16, 27, 20, 36, 23, 44,  5,
		63, 58, 39, 53, 31, 48, 18, 34,
		51, 29, 11, 13, 15, 26, 22, 43,
		57, 38, 47, 17, 28, 10, 25, 21,
		37, 46,  9, 24, 45,  8,  7,  6
	};

	m_pwm_count_1 += value_to_length[data & 0x3f];

	m_pwm_count_total ++;

	if (m_pwm_count_total == 100)
	{
		// The documentation requires:
		// - duty cycle of 9.4%, 305 < rpm < 380 (middle 342.5)
		// - duty cycle of 91%,  625 < rpm < 780 (middle 702.5)
		// - linear between these two points

		int internal_index = m_pwm_count_1 / (m_pwm_count_total/10) - 11;
		if(internal_index < 0)
			internal_index = 0;
		if(internal_index > 399)
			internal_index = 399;

		float duty_cycle = internal_index / 419.0;
		float rpm = (duty_cycle - 0.094) * (702.5 - 342.5) / (0.91 - 0.094) + 342.5;


		// Only change when you get the same value twice consecutively
		// to avoid changing multiple times when in transition.
		if (rpm == m_pwm_current_rpm[1] && m_pwm_current_rpm[1] != m_pwm_current_rpm[0])
		{
			logerror("PWM index %3d duty cycle %5.1f%% rpm %f\n", internal_index, 100*duty_cycle, rpm);

			if (m_cur_floppy && m_cur_floppy->type() == OAD34V)
			{
				m_iwm->sync();
				m_cur_floppy->set_rpm(rpm);
			}
		}

		m_pwm_current_rpm[0] = m_pwm_current_rpm[1];
		m_pwm_current_rpm[1] = rpm;
		m_pwm_count_1 = 0;
		m_pwm_count_total = 0;
	}
}

WRITE_LINE_MEMBER(mac128_state::scsi_irq_w)
{
}

WRITE_LINE_MEMBER(mac128_state::scsi_drq_w)
{
	m_scsi_drq = state;
}

uint16_t mac128_state::macplus_scsi_r(offs_t offset, uint16_t mem_mask)
{
	int reg = (offset >> 3) & 0xf;

	//  logerror("macplus_scsi_r: offset %x mask %x\n", offset, mem_mask);

	if (ACCESSING_BITS_0_7)
	{
		if ((offset >= 0x100) && (m_scsi_drq))
		{
			return m_ncr5380n->dma_r();
		}

		return m_ncr5380n->read(reg);
	}

	if ((offset >= 0x100) && (m_scsi_drq))
	{
		return u16(m_ncr5380n->dma_r()) << 8;
	}

	return u16(m_ncr5380n->read(reg)) << 8;
}

void mac128_state::macplus_scsi_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	int reg = (offset >> 3) & 0xf;

	//  logerror("macplus_scsi_w: data %x offset %x mask %x\n", data, offset, mem_mask);

	// here we can take advantage of 68000 byte smearing
	if ((offset >= 0x100) && (m_scsi_drq))
	{
		m_ncr5380n->dma_w(data & 0xff);
	}

	m_ncr5380n->write(reg, data & 0xff);
}

uint16_t mac128_state::macse_scsi_r(offs_t offset, uint16_t mem_mask)
{
	int reg = (offset>>3) & 0xf;

//  logerror("macplus_scsi_r: offset %x mask %x\n", offset, mem_mask);

	if ((reg == 6) && (offset == 0x130))
	{
		reg = R5380_CURDATA_DTACK;
	}

	return m_ncr5380->ncr5380_read_reg(reg)<<8;
}

void mac128_state::macse_scsi_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	int reg = (offset>>3) & 0xf;

//  logerror("macplus_scsi_w: data %x offset %x mask %x\n", data, offset, mem_mask);

	if ((reg == 0) && (offset == 0x100))
	{
		reg = R5380_OUTDATA_DTACK;
	}

	m_ncr5380->ncr5380_write_reg(reg, data);
}

void mac128_state::scc_mouse_irq(int x, int y)
{
	// DCD lines are active low in hardware but active high to software
	if (x)
	{
		m_scc->dcda_w(m_mouse_last[0] ? 1 : 0);
		if (x < 0)
		{
			m_mouse_bit[0] = m_mouse_last[0] ? 0 : 1;
		}
		else
		{
			m_mouse_bit[0] = m_mouse_last[0] ? 1 : 0;
		}
		m_mouse_last[0] = !m_mouse_last[0];
	}
	if (y)
	{
		m_scc->dcdb_w(m_mouse_last[1] ? 1 : 0);
		if (y < 0)
		{
			m_mouse_bit[1] = m_mouse_last[1] ? 0 : 1;
		}
		else
		{
			m_mouse_bit[1] = m_mouse_last[1] ? 1 : 0;
		}
		m_mouse_last[1] = !m_mouse_last[1];
	}
}

uint16_t mac128_state::mac_iwm_r(offs_t offset, uint16_t mem_mask)
{
	uint16_t result = m_iwm->read((offset >> 8) & 0xf);
	return (result << 8) | result;
}

void mac128_state::mac_iwm_w(offs_t offset, uint16_t data, uint16_t mem_mask)
{
	if (ACCESSING_BITS_0_7)
		m_iwm->write((offset >> 8) & 0xf, data & 0xff);
	else
		m_iwm->write((offset >> 8) & 0xf, data>>8);
}

WRITE_LINE_MEMBER(mac128_state::mac_via_irq)
{
	/* interrupt the 68k (level 1) */
	set_via_interrupt(state);
}

void mac128_state::via_sync()
{
	// The VIA runs from the E clock of the 68k and uses VPA.

	// That means:
	// - The 68000 starts the access cycle, with AS and the address bus.  It's validated at cycle+1.

	// - The glue chip sets VPA.  The 68000 sees it and acts on it at cycle+2.

	// - Between cycle+2 and cycle+11, the E clock goes up.  The VIA
	// is synced on that clock, so that's at a multiple of 10 in
	// absolute time

	// - 4 cycles later E goes down and that's the end of the access,

	// We sync on the start of cycle (so that the via timings go ok)
	// then on the end on via_sync_end()

	uint64_t cur_cycle = m_maincpu->total_cycles();
	uint64_t vpa_cycle = cur_cycle+2;
	uint64_t via_start_cycle = (vpa_cycle + 9) / 10;
	uint64_t m68k_start_cycle = via_start_cycle * 10;
	m_maincpu->adjust_icount(cur_cycle - m68k_start_cycle); // 4 cycles already counted by the core
}

void mac128_state::via_sync_end()
{
	m_maincpu->adjust_icount(-4);
}

uint16_t mac128_state::mac_via_r(offs_t offset)
{
	uint16_t data;

	offset >>= 8;
	offset &= 0x0f;

	via_sync();

	data = m_via->read(offset);

	via_sync_end();
	return (data & 0xff) | (data << 8);
}

void mac128_state::mac_via_w(offs_t offset, uint16_t data)
{
	offset >>= 8;
	offset &= 0x0f;

	via_sync();

	m_via->write(offset, (data >> 8) & 0xff);

	via_sync_end();
}

void mac128_state::mac_autovector_w(offs_t offset, uint16_t data)
{
	/* This should throw an exception */
	/* Not yet implemented */
}

uint16_t mac128_state::mac_autovector_r(offs_t offset)
{
	/* This should throw an exception */
	/* Not yet implemented */
	return 0;
}

uint8_t mac128_state::mac_via_in_a()
{
	return 0x81;
}

uint8_t mac128_state::mac_via_in_b()
{
	int val = 0x40;

	val |= m_mouse_bit[1] << 5; // Mouse Y2
	val |= m_mouse_bit[0] << 4; // Mouse X2
	val |= BIT(~m_mouse0->read(), 0) << 3;

	val |= m_rtc->data_r();

//  printf("%s VIA1 IN_B = %02x\n", machine().describe_context().c_str(), val);

	return val;
}

uint8_t mac128_state::mac_via_in_b_se()
{
	int val = m_macadb->get_adb_state()<<4;

	if (!m_adb_irq_pending)
	{
		val |= 0x08;
	}

	val |= m_rtc->data_r();

//  printf("%s VIA1 IN_B = %02x\n", machine().describe_context().c_str(), val);

	return val;
}

void mac128_state::mac_via_out_a(uint8_t data)
{
//  printf("%s VIA1 OUT A: %02x (PC %x)\n", machine().describe_context().c_str(), data);

	//set_scc_waitrequest((data & 0x80) >> 7);
	m_screen_buffer = (data & 0x40) >> 6;

	int hdsel = BIT(data, 5);
	if (hdsel != m_hdsel)
	{
		if (m_cur_floppy)
		{
			m_cur_floppy->ss_w(hdsel);
		}
		m_hdsel = hdsel;
	}

	m_main_buffer = ((data & 0x08) == 0x08) ? true : false;
	m_snd_vol = data & 0x07;
	update_volume();

	/* Early Mac models had VIA A4 control overlaying.  In the Mac SE and
	 * later models, overlay was set on reset, but cleared on the first
	 * access to the ROM's normal address space. */
	if (((data & 0x10) >> 4) != m_overlay)
	{
		m_overlay = (data & 0x10) >> 4;
	}
}

void mac128_state::mac_via_out_a_se(uint8_t data)
{
//  printf("%s VIA OUT A: %02x (PC %x)\n", machine().describe_context().c_str(), data);

	//set_scc_waitrequest((data & 0x80) >> 7);
	m_screen_buffer = (data & 0x40) >> 6;

	int hdsel = BIT(data, 5);
	if (hdsel != m_hdsel)
	{
		if (m_cur_floppy)
		{
			m_cur_floppy->ss_w(hdsel);
		}
		m_hdsel = hdsel;
	}

	m_snd_vol = data & 0x07;
	update_volume();

	// on SE only this selects which floppy drive (0 = upper, 1 = lower)
	if (m_drive_select != BIT(data, 4))
	{
		m_drive_select = BIT(data, 4);
		devsel_se_w(m_devsel);
	}
}

void mac128_state::mac_via_out_b(uint8_t data)
{
//  printf("%s VIA1 OUT B: %02x\n", machine().describe_context().c_str(), data);

	m_snd_enable = ((data & 0x80) == 0) ? true : false;
	update_volume();
	m_rtc->ce_w((data & 0x04)>>2);
	m_rtc->data_w(data & 0x01);
	m_rtc->clk_w((data >> 1) & 0x01);
}

void mac128_state::mac_via_out_b_se(uint8_t data)
{
//  printf("%s VIA OUT B: %02x\n", machine().describe_context().c_str(), data);

	m_snd_enable = ((data & 0x80) == 0) ? true : false;
	update_volume();

	m_scsiirq_enable = (data & 0x40) ? 0 : 1;

	m_macadb->mac_adb_newaction((data & 0x30) >> 4);

	m_rtc->ce_w((data & 0x04)>>2);
	m_rtc->data_w(data & 0x01);
	m_rtc->clk_w((data >> 1) & 0x01);
}

/* *************************************************************************
 * Mouse
 * *************************************************************************/

void mac128_state::mouse_callback()
{
	// see if it moved in the x coord
	const int new_mx = m_mouse1->read();
	if (new_mx != m_mouse_last_m[0])
	{
		int diff = new_mx - m_mouse_last_m[0];

		// check for wrap
		if (diff > 0x80)
			diff -= 0x100;
		else if (diff < -0x80)
			diff += 0x100;

		m_mouse_count[0] += diff;
		m_mouse_last_m[0] = new_mx;
	}

	// see if it moved in the y coord
	const int new_my = m_mouse2->read();
	if (new_my != m_mouse_last_m[1])
	{
		int diff = new_my - m_mouse_last_m[1];

		// check for wrap
		if (diff > 0x80)
			diff -= 0x100;
		else if (diff < -0x80)
			diff += 0x100;

		m_mouse_count[1] += diff;
		m_mouse_last_m[1] = new_my;
	}

	// update any remaining count and then return
	int x_needs_update = 0;
	if (m_mouse_count[0] < 0)
	{
		m_mouse_count[0]++;
		x_needs_update = -1;
	}
	else if (m_mouse_count[0])
	{
		m_mouse_count[0]--;
		x_needs_update = 1;
	}
	int y_needs_update = 0;
	if (m_mouse_count[1] < 0)
	{
		m_mouse_count[1]++;
		y_needs_update = 1;
	}
	else if (m_mouse_count[1])
	{
		m_mouse_count[1]--;
		y_needs_update = -1;
	}

	if (x_needs_update || y_needs_update)
	{
		// assert Port B External Interrupt on the SCC
		scc_mouse_irq(x_needs_update, y_needs_update);
	}
}

void mac128_state::mac_driver_init()
{
	m_scsi_interrupt = 0;

	memset(m_ram->pointer(), 0, m_ram->size());
}

#define MAC_MAIN_SCREEN_BUF_OFFSET  (0x5900>>1)
#define MAC_ALT_SCREEN_BUF_OFFSET   (0xD900>>1)

uint32_t mac128_state::screen_update_mac(screen_device &screen, bitmap_ind16 &bitmap, const rectangle &cliprect)
{
	uint32_t const video_base = m_ram_size - (m_screen_buffer ? MAC_MAIN_SCREEN_BUF_OFFSET : MAC_ALT_SCREEN_BUF_OFFSET);
	uint16_t const *video_ram = (const uint16_t *) (m_ram_ptr + video_base);

	for (int y = 0; y < MAC_V_VIS; y++)
	{
		uint16_t *const line = &bitmap.pix(y);

		for (int x = 0; x < MAC_H_VIS; x += 16)
		{
			uint16_t const word = *(video_ram++);
			for (int b = 0; b < 16; b++)
			{
				line[x + b] = (word >> (15 - b)) & 0x0001;
			}
		}
	}
	return 0;
}


void mac128_state::phases_w(uint8_t phases)
{
	if (m_cur_floppy)
		m_cur_floppy->seek_phase_w(phases);
}

void mac128_state::devsel_w(uint8_t devsel)
{
	if (devsel == 1)
		m_cur_floppy = m_floppy[0]->get_device();
	else if (devsel == 2)
		m_cur_floppy = m_floppy[1]->get_device();
	else
		m_cur_floppy = nullptr;

	m_iwm->set_floppy(m_cur_floppy);
	if (m_cur_floppy)
	{
		m_cur_floppy->ss_w(m_hdsel);
		if (m_cur_floppy->type() == OAD34V)
			m_cur_floppy->set_rpm(m_pwm_current_rpm[1]);
	}
}

void mac128_state::devsel_se_w(uint8_t devsel)
{
	// m_drive_select = 0 for the SE's two internal drives, 1 for the single external
	if (!m_drive_select)
	{
		if (devsel == 1)
			m_cur_floppy = m_floppy[0]->get_device();
		else if (devsel == 2)
			m_cur_floppy = m_floppy[1]->get_device();
		else
			m_cur_floppy = nullptr;
	}
	else
	{
		m_cur_floppy = nullptr;
	}

	m_iwm->set_floppy(m_cur_floppy);
	if (m_cur_floppy)
	{
		m_cur_floppy->ss_w(m_hdsel);
		if (m_cur_floppy->type() == OAD34V)
			m_cur_floppy->set_rpm(m_pwm_current_rpm[1]);
	}

	m_devsel = devsel;
}

/***************************************************************************
    ADDRESS MAPS
***************************************************************************/

void mac128_state::mac512ke_map(address_map &map)
{
	map(0x000000, 0x3fffff).rw(FUNC(mac128_state::ram_r), FUNC(mac128_state::ram_w));
	map(0x400000, 0x4fffff).rom().region("bootrom", 0).mirror(0x100000);
	map(0x600000, 0x6fffff).rw(FUNC(mac128_state::ram_600000_r), FUNC(mac128_state::ram_600000_w));
	map(0x800000, 0x9fffff).r(m_scc, FUNC(z80scc_device::dc_ab_r)).umask16(0xff00);
	map(0xa00000, 0xbfffff).w(m_scc, FUNC(z80scc_device::dc_ab_w)).umask16(0x00ff);
	map(0xc00000, 0xdfffff).rw(FUNC(mac128_state::mac_iwm_r), FUNC(mac128_state::mac_iwm_w));
	map(0xe80000, 0xefffff).rw(FUNC(mac128_state::mac_via_r), FUNC(mac128_state::mac_via_w));
	map(0xfffff0, 0xffffff).rw(FUNC(mac128_state::mac_autovector_r), FUNC(mac128_state::mac_autovector_w));
}

void mac128_state::macplus_map(address_map &map)
{
	map(0x000000, 0x3fffff).rw(FUNC(mac128_state::ram_r), FUNC(mac128_state::ram_w));
	map(0x400000, 0x4fffff).rom().region("bootrom", 0);
	map(0x580000, 0x5fffff).rw(FUNC(mac128_state::macplus_scsi_r), FUNC(mac128_state::macplus_scsi_w));
	map(0x800000, 0x9fffff).r(m_scc, FUNC(z80scc_device::dc_ab_r)).umask16(0xff00);
	map(0xa00000, 0xbfffff).w(m_scc, FUNC(z80scc_device::dc_ab_w)).umask16(0x00ff);
	map(0xc00000, 0xdfffff).rw(FUNC(mac128_state::mac_iwm_r), FUNC(mac128_state::mac_iwm_w));
	map(0xe80000, 0xefffff).rw(FUNC(mac128_state::mac_via_r), FUNC(mac128_state::mac_via_w));
	map(0xfffff0, 0xffffff).rw(FUNC(mac128_state::mac_autovector_r), FUNC(mac128_state::mac_autovector_w));
}

void mac128_state::macse_map(address_map &map)
{
	map(0x000000, 0x3fffff).rw(FUNC(mac128_state::ram_r), FUNC(mac128_state::ram_w_se));
	map(0x400000, 0x4fffff).rom().region("bootrom", 0);
	map(0x580000, 0x5fffff).rw(FUNC(mac128_state::macse_scsi_r), FUNC(mac128_state::macse_scsi_w));
	map(0x900000, 0x9fffff).r(m_scc, FUNC(z80scc_device::dc_ab_r)).umask16(0xff00);
	map(0xb00000, 0xbfffff).w(m_scc, FUNC(z80scc_device::dc_ab_w)).umask16(0x00ff);
	map(0xd00000, 0xdfffff).rw(FUNC(mac128_state::mac_iwm_r), FUNC(mac128_state::mac_iwm_w));
	map(0xe80000, 0xefffff).rw(FUNC(mac128_state::mac_via_r), FUNC(mac128_state::mac_via_w));
	map(0xfffff0, 0xffffff).rw(FUNC(mac128_state::mac_autovector_r), FUNC(mac128_state::mac_autovector_w));
}

/***************************************************************************
    MACHINE DRIVERS
***************************************************************************/

static void mac_scsi_devices(device_slot_interface &device)
{
	device.option_add("harddisk", NSCSI_HARDDISK);
	device.option_add("cdrom", NSCSI_CDROM);
}

static void mac_pds_cards(device_slot_interface &device)
{
	device.option_add("hyperdrive", PDS_HYPERDRIVE);  // GCC HyperDrive ST-506 interface
}

void mac128_state::mac512ke(machine_config &config)
{
	/* basic machine hardware */
	M68000(config, m_maincpu, C7M);        /* 7.8336 MHz */
	m_maincpu->set_addrmap(AS_PROGRAM, &mac128_state::mac512ke_map);
	config.set_maximum_quantum(attotime::from_hz(60));

	/* video hardware */
	SCREEN(config, m_screen, SCREEN_TYPE_RASTER);
	m_screen->set_raw(15.6672_MHz_XTAL, MAC_H_TOTAL, 0, MAC_H_VIS, MAC_V_TOTAL, 0, MAC_V_VIS);
	m_screen->set_screen_update(FUNC(mac128_state::screen_update_mac));
	m_screen->screen_vblank().set(FUNC(mac128_state::vblank_w));
	m_screen->set_palette("palette");

	PALETTE(config, "palette", palette_device::MONOCHROME_INVERTED);

	/* sound hardware */
	SPEAKER(config, "speaker").front_center();
	DAC_8BIT_PWM(config, m_dac, 0).add_route(ALL_OUTPUTS, "speaker", 0.25); // 2 x ls161

	/* devices */
	RTC3430042(config, m_rtc, 32.768_kHz_XTAL);

	IWM(config, m_iwm, C7M);
	m_iwm->phases_cb().set(FUNC(mac128_state::phases_w));
	m_iwm->devsel_cb().set(FUNC(mac128_state::devsel_w));

	applefdintf_device::add_35(config, m_floppy[0]);
	applefdintf_device::add_35(config, m_floppy[1]);

	SCC85C30(config, m_scc, C7M);
	m_scc->configure_channels(C3_7M, 0, C3_7M, 0);
	m_scc->out_int_callback().set(FUNC(mac128_state::set_scc_interrupt));

	MOS6522(config, m_via, C7M/10);
	m_via->readpa_handler().set(FUNC(mac128_state::mac_via_in_a));
	m_via->readpb_handler().set(FUNC(mac128_state::mac_via_in_b));
	m_via->writepa_handler().set(FUNC(mac128_state::mac_via_out_a));
	m_via->writepb_handler().set(FUNC(mac128_state::mac_via_out_b));
	m_via->cb2_handler().set(m_mackbd, FUNC(mac_keyboard_port_device::data_w));
	m_via->irq_handler().set(FUNC(mac128_state::mac_via_irq));

	MAC_KEYBOARD_PORT(config, m_mackbd, mac_keyboard_devices, "pad");
	m_mackbd->clock_cb().set(m_via, FUNC(via6522_device::write_cb1));
	m_mackbd->data_cb().set(m_via, FUNC(via6522_device::write_cb2));

	/* internal ram */
	RAM(config, m_ram);
	m_ram->set_default_size("512K");

	MACPDS(config, "macpds", "maincpu");
	MACPDS_SLOT(config, "pds", "macpds", mac_pds_cards, nullptr);

	// software list
	SOFTWARE_LIST(config, "flop35_list").set_original("mac_flop");
	SOFTWARE_LIST(config, "hdd_list").set_original("mac_hdd");
}

void mac128_state::mac128k(machine_config &config)
{
	mac512ke(config);
	m_ram->set_default_size("128K");

	IWM(config.replace(), m_iwm, C7M);
	m_iwm->phases_cb().set(FUNC(mac128_state::phases_w));
	m_iwm->devsel_cb().set(FUNC(mac128_state::devsel_w));

	applefdintf_device::add_35_sd(config, m_floppy[0]);
	applefdintf_device::add_35_sd(config, m_floppy[1]);
}

void mac128_state::mac512k(machine_config &config)
{
	mac128k(config);
	m_ram->set_default_size("512K");
}

void mac128_state::macplus(machine_config &config)
{
	mac512ke(config);
	m_maincpu->set_addrmap(AS_PROGRAM, &mac128_state::macplus_map);

	m_mackbd->set_default_option("usp");

	// SCSI bus and devices
	NSCSI_BUS(config, m_scsibus);
	NSCSI_CONNECTOR(config, "scsibus:0", mac_scsi_devices, nullptr);
	NSCSI_CONNECTOR(config, "scsibus:1", mac_scsi_devices, nullptr);
	NSCSI_CONNECTOR(config, "scsibus:2", mac_scsi_devices, nullptr);
	NSCSI_CONNECTOR(config, "scsibus:3", mac_scsi_devices, nullptr);
	NSCSI_CONNECTOR(config, "scsibus:4", mac_scsi_devices, nullptr);
	NSCSI_CONNECTOR(config, "scsibus:5", mac_scsi_devices, nullptr);
	NSCSI_CONNECTOR(config, "scsibus:6", mac_scsi_devices, "harddisk");
	NSCSI_CONNECTOR(config, "scsibus:7").option_set("ncr5380n", NCR5380N).machine_config([this](device_t *device) {
		ncr5380n_device &adapter = downcast<ncr5380n_device &>(*device);
		adapter.irq_handler().set(*this, FUNC(mac128_state::scsi_irq_w));
		adapter.drq_handler().set(*this, FUNC(mac128_state::scsi_drq_w));
	});

	/* internal ram */
	m_ram->set_default_size("4M");
	m_ram->set_extra_options("1M,2M,2560K,4M");
}

static void mac_sepds_cards(device_slot_interface &device)
{
	device.option_add("radiusfpd", PDS_SEDISPLAY);  // Radius Full Page Display card for SE
}

void mac128_state::macse(machine_config &config)
{
	macplus(config);
	m_maincpu->set_addrmap(AS_PROGRAM, &mac128_state::macse_map);

	config.device_remove("kbd");
	config.device_remove("pds");
	config.device_remove("scsibus");

	IWM(config.replace(), m_iwm, C7M*2);
	m_iwm->phases_cb().set(FUNC(mac128_state::phases_w));
	m_iwm->devsel_cb().set(FUNC(mac128_state::devsel_se_w));

	applefdintf_device::add_35(config, m_floppy[0]);
	applefdintf_device::add_35(config, m_floppy[1]);

	scsi_port_device &scsibus(SCSI_PORT(config, "scsi"));
	scsibus.set_slot_device(1, "harddisk", SCSIHD, DEVICE_INPUT_DEFAULTS_NAME(SCSI_ID_6));
	scsibus.set_slot_device(2, "harddisk", SCSIHD, DEVICE_INPUT_DEFAULTS_NAME(SCSI_ID_5));

	NCR5380(config, m_ncr5380, C7M);
	m_ncr5380->set_scsi_port("scsi");
	m_ncr5380->irq_callback().set(FUNC(mac128_state::scsi_irq_w));

	MACADB(config, m_macadb, C7M);
	m_macadb->via_clock_callback().set(m_via, FUNC(via6522_device::write_cb1));
	m_macadb->via_data_callback().set(m_via, FUNC(via6522_device::write_cb2));
	m_macadb->adb_irq_callback().set(FUNC(mac128_state::adb_irq_w));

	R65NC22(config.replace(), m_via, C7M/10);
	m_via->readpa_handler().set(FUNC(mac128_state::mac_via_in_a));
	m_via->readpb_handler().set(FUNC(mac128_state::mac_via_in_b_se));
	m_via->writepa_handler().set(FUNC(mac128_state::mac_via_out_a_se));
	m_via->writepb_handler().set(FUNC(mac128_state::mac_via_out_b_se));
	m_via->cb2_handler().set(m_macadb, FUNC(macadb_device::adb_data_w));
	m_via->irq_handler().set(FUNC(mac128_state::mac_via_irq));

	/* internal ram */
	m_ram->set_default_size("4M");
	m_ram->set_extra_options("2M,2560K,4M");

	MACPDS(config, "sepds", "maincpu");
	MACPDS_SLOT(config, "pds", "sepds", mac_sepds_cards, nullptr);
}

void mac128_state::macsefd(machine_config &config)
{
	macse(config);

	SWIM1(config.replace(), m_iwm, C7M*2);
	m_iwm->phases_cb().set(FUNC(mac128_state::phases_w));
	m_iwm->devsel_cb().set(FUNC(mac128_state::devsel_se_w));

	applefdintf_device::add_35_hd(config, m_floppy[0]);
	applefdintf_device::add_35_hd(config, m_floppy[1]);
}

void mac128_state::macclasc(machine_config &config)
{
	macsefd(config);

	config.device_remove("pds");
	config.device_remove("sepds");
}

static INPUT_PORTS_START( macplus )
	PORT_START("MOUSE0") /* Mouse - button */
	PORT_BIT( 0x01, IP_ACTIVE_HIGH, IPT_BUTTON1) PORT_NAME("Mouse Button") PORT_CODE(MOUSECODE_BUTTON1)

	PORT_START("MOUSE1") /* Mouse - X AXIS */
	PORT_BIT( 0xff, 0x00, IPT_MOUSE_X) PORT_SENSITIVITY(100) PORT_KEYDELTA(0)

	PORT_START("MOUSE2") /* Mouse - Y AXIS */
	PORT_BIT( 0xff, 0x00, IPT_MOUSE_Y) PORT_SENSITIVITY(100) PORT_KEYDELTA(0)
INPUT_PORTS_END

static INPUT_PORTS_START( macadb )
INPUT_PORTS_END

/***************************************************************************

  Machine driver(s)

***************************************************************************/

/*
ROM_START( mactw )
    ROM_REGION16_BE(0x100000, "bootrom", 0)
    ROM_LOAD( "rom4.3t_07-04-83.bin", 0x0000, 0x10000, CRC(d2c42f18) SHA1(f868c09ca70383a69751c37a5a3110a9597462a4) )
ROM_END
*/

ROM_START( mac128k )
	ROM_REGION16_BE(0x100000, "bootrom", 0)
	// Apple used at least 3 manufacturers for these ROMs, but they're always Apple part numbers 342-0220-A and 342-0221-A
	ROMX_LOAD("342-0220-a.u6d",  0x00000, 0x08000, CRC(198210ad) SHA1(2590ff4af5ac0361babdf0dc5da18e2eecad454a), ROM_SKIP(1) )
	ROMX_LOAD("342-0221-a.u8d",  0x00001, 0x08000, CRC(fd2665c2) SHA1(8507932a854bd28196a17785c8b1851cb53eaf64), ROM_SKIP(1) )
	/* Labels seen in the wild:
	VTi:
	"<VTi logo along side> // 416 VH 2605 // 23256-1020 // 342-0220-A // (C)APPLE 83 // KOREA-AE"
	"<VTi logo along side> // 416 VH 2826 // 23256-1023 // 342-0221-A // (C)APPLE 83 // KOREA-AE"
	Synertek:
	"<Synertek 'S' logo> 8416 G // C19728 // 342-0220-A // (C)APPLE 83"
	"<Synertek 'S' logo> 8410 G // C19729 // 342-0221-A // (C)APPLE 83"
	Hitachi:
	[can't find reference for rom-hi]
	"<Hitachi 'target' logo> 8413 // 3256 016 JAPAN // (C)APPLE 83 // 342-0221-A"

	References:
	http://www.vintagecomputer.net/apple/Macintosh/Macintosh_motherboard.jpg
	https://upload.wikimedia.org/wikipedia/commons/3/34/Macintosh-motherboard.jpg
	https://68kmla.org/forums/uploads/monthly_01_2016/post-2105-0-31195100-1452296677.jpg
	https://68kmla.org/forums/uploads/monthly_12_2014/post-2597-0-46269000-1419299800.jpg
	http://cdn.cultofmac.com/wp-content/uploads/2014/01/12A-128k-Motherboard.jpg
	*/
ROM_END

ROM_START( mac512k )
	ROM_REGION16_BE(0x100000, "bootrom", 0)
	ROMX_LOAD("342-0220-b.u6d",  0x00000, 0x08000, CRC(0dce9a3f) SHA1(101ca6570f5a273e400d1a8bc63e15ee0e94153e), ROM_SKIP(1) ) // "<VTi logo along side> 512 VH 6434 // 23256-1104 // 342-0220-B // (C) APPLE 84 // KOREA-A"
	ROMX_LOAD("342-0221-b.u8d",  0x00001, 0x08000, CRC(d51f376e) SHA1(575586109e876cffa4a4d472cb38771aa21b70cb), ROM_SKIP(1) ) // "<VTi logo along side> 512 VH 6709 // 23256-1105 // 342-0221-B // (C) APPLE 84 // KOREA-A"
	// reference: http://i.ebayimg.com/images/g/Uj8AAOSwvzRXy2tW/s-l1600.jpg
ROM_END

ROM_START( unitron )
	ROM_REGION16_BE(0x100000, "bootrom", 0)
	ROM_LOAD16_WORD( "unitron_512.rom", 0x00000, 0x10000, CRC(1eabd37f) SHA1(a3d3696c08feac6805effb7ee07b68c2bf1a8dd7) )
ROM_END

ROM_START( utrn1024 )
	ROM_REGION16_BE(0x100000, "bootrom", 0)
	// CRCs match the original "Lonely Hearts" version 1 Mac Plus ROM: 4d1eeee1
	ROMX_LOAD( "342-0341-a.u6d", 0x000000, 0x010000, CRC(5095fe39) SHA1(be780580033d914b5035d60b5ebbd66bd1d28a9b), ROM_SKIP(1) ) // not correct label
	ROMX_LOAD( "342-0342-a.u8d", 0x000001, 0x010000, CRC(fb766270) SHA1(679f529fbfc05f9cc98924c53457d2996dfcb1a7), ROM_SKIP(1) ) // not correct label
ROM_END

ROM_START( mac512ke ) // 512ke has been observed with any of the v3, v2 or v1 macplus romsets installed, and v1 romsets are more common here than in the plus, since the 512ke lacks scsi, which is the cause of the major bug fixed between v1 and v2, hence 512ke is unaffected and was a good way for apple to use up the buggy roms rather than destroying them.
	ROM_REGION16_BE(0x100000, "bootrom", 0)
	ROM_SYSTEM_BIOS(0, "v3", "Loud Harmonicas")
	ROMX_LOAD( "342-0341-c.u6d", 0x000000, 0x010000, CRC(f69697e6) SHA1(41317614ac71eb94941e9952f6ea37407e21ffff), ROM_SKIP(1) | ROM_BIOS(0) )
	ROMX_LOAD( "342-0342-b.u8d", 0x000001, 0x010000, CRC(49f25913) SHA1(72f658c02bae265e8845899582575fb7c784ee87), ROM_SKIP(1) | ROM_BIOS(0) )
	ROM_FILL(0x20000, 0x2, 0xff)    // ROM checks for same contents at 20000 and 40000 to determine if SCSI is present
	ROM_FILL(0x40000, 0x2, 0xaa)
	ROM_SYSTEM_BIOS(1, "v2", "Lonely Heifers")
	ROMX_LOAD( "342-0341-b.u6d", 0x000000, 0x010000, CRC(65341487) SHA1(bf43fa4f5a3dcbbac20f1fe1deedee0895454379), ROM_SKIP(1) | ROM_BIOS(1) )
	ROMX_LOAD( "342-0342-a.u8d", 0x000001, 0x010000, CRC(fb766270) SHA1(679f529fbfc05f9cc98924c53457d2996dfcb1a7), ROM_SKIP(1) | ROM_BIOS(1) )
	ROM_FILL(0x20000, 0x2, 0xff)
	ROM_FILL(0x40000, 0x2, 0xaa)
	ROM_SYSTEM_BIOS(2, "v1", "Lonely Hearts")
	ROMX_LOAD( "342-0341-a.u6d", 0x000000, 0x010000, CRC(5095fe39) SHA1(be780580033d914b5035d60b5ebbd66bd1d28a9b), ROM_SKIP(1) | ROM_BIOS(2) )
	ROMX_LOAD( "342-0342-a.u8d", 0x000001, 0x010000, CRC(fb766270) SHA1(679f529fbfc05f9cc98924c53457d2996dfcb1a7), ROM_SKIP(1) | ROM_BIOS(2) )
	ROM_FILL(0x20000, 0x2, 0xff)
	ROM_FILL(0x40000, 0x2, 0xaa)
	/* from Technical note HW11 (https://www.fenestrated.net/mirrors/Apple%20Technotes%20(As%20of%202002)/hw/hw_11.html)
	1st version (Lonely Hearts, checksum 4D 1E EE E1)
	Bug in the SCSI driver; won't boot if external drive is turned off. We only produced about
	one and a half months worth of these.

	2nd version (Lonely Heifers, checksum 4D 1E EA E1):
	Fixed boot bug. This version is the vast majority of beige Macintosh Pluses.

	3rd version (Loud Harmonicas, checksum 4D 1F 81 72):
	Fixed bug for drives that return Unit Attention on power up or reset. Basically took the
	SCSI bus Reset command out of the boot sequence loop, so it will only reset once
	during boot sequence.
	*/
	/* Labels seen in the wild:
	v3/4d1f8172:
	    'ROM-HI' @ U6D:
	        "VLSI // 740 SA 1262 // 23512-1054 // 342-0341-C // (C)APPLE '83-'86 // KOREA A"
	        "342-0341-C // (C)APPLE 85,86 // (M)AMI 8849MBL // PHILLIPINES"
	    'ROM-LO' @ U8D:
	        "VLSI // 740 SA 1342 // 23512-1055 // 342-0342-B // (C)APPLE '83-'86 // KOREA A"
	        "<VLSI logo>VLSI // 8905AV 0 AS759 // 23512-1055 // 342-0342-B // (C)APPLE '85-'86"
	v2/4d1eeae1:
	    'ROM-HI' @ U6D:
	        "VTI // 624 V0 8636 // 23512-1010 // 342-0341-B // (C)APPLE '85 // MEXICO R"
	    'ROM-LO' @ U8D:
	        "VTI // 622 V0 B637 // 23512-1007 // 342-0342-A // (C)APPLE '83-'85 // KOREA A"
	v1/4d1eeee1:
	    'ROM-HI' @ U6D:
	        GUESSED, since this ROM is very rare: "VTI // 62? V0 86?? // 23512-1008 // 342-0341-A // (C)APPLE '83-'85 // KOREA A"
	    'ROM-LO' @ U8D is same as v2/4d1eeae1 'ROM-LO' @ U8D
	*/
ROM_END

ROM_START( macplus ) // same notes as above apply here as well
	ROM_REGION16_BE(0x100000, "bootrom", 0)
	ROM_SYSTEM_BIOS(0, "v3", "Loud Harmonicas")
	ROMX_LOAD( "342-0341-c.u6d", 0x000000, 0x010000, CRC(f69697e6) SHA1(41317614ac71eb94941e9952f6ea37407e21ffff), ROM_SKIP(1) | ROM_BIOS(0) )
	ROMX_LOAD( "342-0342-b.u8d", 0x000001, 0x010000, CRC(49f25913) SHA1(72f658c02bae265e8845899582575fb7c784ee87), ROM_SKIP(1) | ROM_BIOS(0) )
	ROM_FILL(0x20000, 0x2, 0xff)    // ROM checks for same contents at 20000 and 40000 to determine if SCSI is present
	ROM_FILL(0x40000, 0x2, 0xaa)
	ROM_SYSTEM_BIOS(1, "v2", "Lonely Heifers")
	ROMX_LOAD( "342-0341-b.u6d", 0x000000, 0x010000, CRC(65341487) SHA1(bf43fa4f5a3dcbbac20f1fe1deedee0895454379), ROM_SKIP(1) | ROM_BIOS(1) )
	ROMX_LOAD( "342-0342-a.u8d", 0x000001, 0x010000, CRC(fb766270) SHA1(679f529fbfc05f9cc98924c53457d2996dfcb1a7), ROM_SKIP(1) | ROM_BIOS(1) )
	ROM_FILL(0x20000, 0x2, 0xff)
	ROM_FILL(0x40000, 0x2, 0xaa)
	ROM_SYSTEM_BIOS(2, "v1", "Lonely Hearts")
	ROMX_LOAD( "342-0341-a.u6d", 0x000000, 0x010000, CRC(5095fe39) SHA1(be780580033d914b5035d60b5ebbd66bd1d28a9b), ROM_SKIP(1) | ROM_BIOS(2) )
	ROMX_LOAD( "342-0342-a.u8d", 0x000001, 0x010000, CRC(fb766270) SHA1(679f529fbfc05f9cc98924c53457d2996dfcb1a7), ROM_SKIP(1) | ROM_BIOS(2) )
	ROM_FILL(0x20000, 0x2, 0xff)
	ROM_FILL(0x40000, 0x2, 0xaa)
	ROM_SYSTEM_BIOS(3, "romdisk", "mac68k.info self-boot (1/1/2015)")
	ROMX_LOAD( "modplus-harp2.bin", 0x000000, 0x028000, CRC(ba56078d) SHA1(debdf328ac73e1662d274a044d8750224f47edef), ROM_GROUPWORD | ROM_BIOS(3) )
	ROM_SYSTEM_BIOS(4, "romdisk2", "bigmessofwires.com ROMinator (2/25/2015)")
	ROMX_LOAD( "rominator-20150225-lo.bin", 0x000001, 0x080000, CRC(62cf2a0b) SHA1(f78ebb0919dd9e094bef7952b853b70e66d05e01), ROM_SKIP(1) | ROM_BIOS(4) )
	ROMX_LOAD( "rominator-20150225-hi.bin", 0x000000, 0x080000, CRC(a28ba8ec) SHA1(9ddcf500727955c60db0ff24b5ca2458f53fd89a), ROM_SKIP(1) | ROM_BIOS(4) )
ROM_END

ROM_START( macse )
	ROM_REGION16_BE(0x100000, "bootrom", 0)
	ROM_LOAD16_WORD( "macse.rom",  0x00000, 0x40000, CRC(0f7ff80c) SHA1(58532b7d0d49659fd5228ac334a1b094f0241968))
ROM_END

ROM_START( macsefd )
	ROM_REGION16_BE(0x100000, "bootrom", 0)
	ROM_LOAD( "be06e171.rom", 0x000000, 0x040000, CRC(f530cb10) SHA1(d3670a90273d12e53d86d1228c068cb660b8c9d1) )
ROM_END

ROM_START( macclasc )
	ROM_REGION16_BE(0x100000, "bootrom", 0) // a49f9914, second half of chip dump is the 6.0.3 XO rom disk
	// this dump is big endian
	ROM_LOAD( "341-0813__=c=1983-90_apple__japan__910d_d.27c4096_be.ue1", 0x000000, 0x080000, CRC(510d7d38) SHA1(ccd10904ddc0fb6a1d216b2e9effd5ec6cf5a83d) )
ROM_END

/*    YEAR  NAME      PARENT   COMPAT  MACHINE   INPUT    CLASS         INIT              COMPANY              FULLNAME */
//COMP( 1983, mactw,    0,       0,      mac128k,  macplus, mac128_state, mac_driver_init, "Apple Computer",    "Macintosh (4.3T Prototype)",  MACHINE_SUPPORTS_SAVE )
COMP( 1984, mac128k,  0,       0,      mac128k,  macplus, mac128_state, mac_driver_init,  "Apple Computer",    "Macintosh 128k",  MACHINE_SUPPORTS_SAVE )
COMP( 1984, mac512k,  mac128k, 0,      mac512k,  macplus, mac128_state, mac_driver_init,  "Apple Computer",    "Macintosh 512k",  MACHINE_SUPPORTS_SAVE )
COMP( 1986, mac512ke, macplus, 0,      mac512ke, macplus, mac128_state, mac_driver_init,  "Apple Computer",    "Macintosh 512ke", MACHINE_SUPPORTS_SAVE )
COMP( 1985, unitron,  macplus, 0,      mac512ke, macplus, mac128_state, mac_driver_init,  "bootleg (Unitron)", "Mac 512",  MACHINE_SUPPORTS_SAVE )
COMP( 1986, macplus,  0,       0,      macplus,  macplus, mac128_state, mac_driver_init,  "Apple Computer",    "Macintosh Plus",  MACHINE_SUPPORTS_SAVE )
COMP( 1985, utrn1024, macplus, 0,      macplus,  macplus, mac128_state, mac_driver_init,  "bootleg (Unitron)", "Unitron 1024",  MACHINE_SUPPORTS_SAVE )
COMP( 1987, macse,    0,       0,      macse,    macadb, mac128_state,  mac_driver_init,  "Apple Computer",   "Macintosh SE",  MACHINE_SUPPORTS_SAVE )
COMP( 1987, macsefd,  0,       0,      macsefd,  macadb, mac128_state,  mac_driver_init,  "Apple Computer",   "Macintosh SE (FDHD)",  MACHINE_SUPPORTS_SAVE )
COMP( 1990, macclasc, 0,       0,      macclasc, macadb, mac128_state,  mac_driver_init,  "Apple Computer",   "Macintosh Classic",  MACHINE_SUPPORTS_SAVE )
