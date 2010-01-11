#include "Nes_Vrc7_Apu.h"

#include "emu2413.h"
#include <string.h>

#include "blargg_source.h"

int const period = 36; // NES CPU clocks per FM clock

Nes_Vrc7_Apu::Nes_Vrc7_Apu()
{
	opll = 0;
}

blargg_err_t Nes_Vrc7_Apu::init()
{
	CHECK_ALLOC( opll = VRC7_new( 3579545 ) );
	
	output( 0 );
	volume( 1.0 );
	reset();
	return 0;
}

Nes_Vrc7_Apu::~Nes_Vrc7_Apu()
{
	if ( opll )
		VRC7_delete( opll );
}

void Nes_Vrc7_Apu::output( Blip_Buffer* buf )
{
	for ( int i = 0; i < osc_count; i++ )
		oscs [i].output = buf;
	output_changed();
}

void Nes_Vrc7_Apu::output_changed()
{
	mono.output = oscs [0].output;
	for ( int i = 1; i < osc_count; i++ )
	{
		if ( mono.output != oscs [i].output )
		{
			mono.output = 0;
			break;
		}
	}
	
	if ( mono.output )
	{
		for ( int i = 1; i < osc_count; i++ )
		{
			mono.last_amp += oscs [i].last_amp;
			oscs [i].last_amp = 0;
		}
	}
}

void Nes_Vrc7_Apu::reset()
{
	addr      = 0;
	next_time = 0;
	mono.last_amp = 0;
	
	for ( int i = 0; i < osc_count; ++i )
	{
		Vrc7_Osc& osc = oscs [i];
		osc.last_amp = 0;
		for ( int j = 0; j < 3; ++j )
			osc.regs [j] = 0;
	}

	VRC7_reset( opll );
}

void Nes_Vrc7_Apu::write_reg( int data )
{
	addr = data;
}

void Nes_Vrc7_Apu::write_data( nes_time_t time, int data )
{
	int type = (addr >> 4) - 1;
	int chan = addr & 15;
	if ( (unsigned) type < 3 && chan < osc_count )
		oscs [chan].regs [type] = data;
	
	if ( time > next_time )
		run_until( time );
	VRC7_writeReg( opll, addr, data );
}

void Nes_Vrc7_Apu::end_frame( nes_time_t time )
{
	if ( time > next_time )
		run_until( time );
	
	next_time -= time;
	assert( next_time >= 0 );
}

void Nes_Vrc7_Apu::save_snapshot( vrc7_snapshot_t* out ) const
{
	out->latch = addr;
	out->delay = next_time;
	for ( int i = 0; i < osc_count; ++i )
	{
		for ( int j = 0; j < 3; ++j )
			out->regs [i] [j] = oscs [i].regs [j];
	}
	memcpy( out->inst, opll->CustInst, 8 );
}

void Nes_Vrc7_Apu::load_snapshot( vrc7_snapshot_t const& in )
{
	assert( offsetof (vrc7_snapshot_t,delay) == 28 - 1 );
	
	reset();
	next_time = in.delay;
	write_reg( in.latch );
	int i;
	for ( i = 0; i < osc_count; ++i )
	{
		for ( int j = 0; j < 3; ++j )
			oscs [i].regs [j] = in.regs [i] [j];
	}

	for ( i = 0; i < 8; ++i )
		VRC7_writeReg( opll, i, in.inst [i] );

	for ( i = 0; i < 3; ++i )
	{
		for ( int j = 0; j < 6; ++j )
			VRC7_writeReg( opll, 0x10 + i * 0x10 + j, oscs [j].regs [i] );
	}
}

void Nes_Vrc7_Apu::run_until( nes_time_t end_time )
{
	require( end_time > next_time );

	nes_time_t time = next_time;
	OPLL* const opll = this->opll; // cache
	Blip_Buffer* const mono_output = mono.output;
	if ( mono_output )
	{
		mono_output->set_modified();
		// optimal case
		do
		{
			VRC7_run( opll );
			int amp = 0;
			for ( int i = 0; i < osc_count; i++ )
				amp += VRC7_calcCh( opll, i );
			int delta = amp - mono.last_amp;
			if ( delta )
			{
				mono.last_amp = amp;
				synth.offset_inline( time, delta, mono_output );
			}
			time += period;
		}
		while ( time < end_time );
	}
	else
	{
		for ( int i = osc_count; --i >= 0; )
		{
			Blip_Buffer* output = oscs [i].output;
			if ( output )
				output->set_modified();
		}
		
		mono.last_amp = 0;
		do
		{
			VRC7_run( opll );
			for ( int i = 0; i < osc_count; ++i )
			{
				Vrc7_Osc& osc = oscs [i];
				if ( osc.output )
				{
					int amp = VRC7_calcCh( opll, i );
					int delta = amp - osc.last_amp;
					if ( delta )
					{
						osc.last_amp = amp;
						synth.offset( time, delta, osc.output );
					}
				}
			}
			time += period;
		}
		while ( time < end_time );
	}
	next_time = time;
}
