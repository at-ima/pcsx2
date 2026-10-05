// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>

// Decides whether the GS thread runs on the efficiency cores, one window of
// frames at a time, from the window's load (GS work over wall time) and
// whether emulation ran below full speed in it. A GS that keeps up sleeps
// waiting for work, so a load near 1 means it fell behind. Below full speed
// (a GPU-bound scene, say, where the GS mostly waits for the GPU) the GS
// should not add latency either. (The EE waiting for the GS at vsync is no
// sign: it does that nearly every frame on the performance cores too.)
//
// On the efficiency cores, a load above PromoteLoad or a slow window moves the
// thread back to the performance cores, where it stays for a hold time that doubles with
// every move back made soon after the last one. There, once the average load
// times the slowdown between the cores stays below DemoteLoad for
// CalmWindows, it moves to the efficiency cores.
// The slowdown is the average, over the windows on the efficiency cores, of
// their load over the average load on the performance cores before the move.
// MTGS::AdjustCores() measures the windows and applies the decision.
class GSThreadCorePolicy
{
public:
	static constexpr double PromoteLoad = 0.90;
	static constexpr double DemoteLoad = 0.80;
	static constexpr unsigned CalmWindows = 4;
	static constexpr unsigned HoldWindows = 20;
	static constexpr unsigned MaxHoldWindows = 16 * HoldWindows;
	// An efficiency-core stint this long resets the hold time.
	static constexpr unsigned SettledWindows = 60;

	bool Efficiency() const { return m_efficiency; }
	double Slowdown() const { return m_slowdown; }
	unsigned HoldTime() const { return m_hold_time; }

	// Takes one window; returns whether the thread should be on the efficiency
	// cores from now on.
	bool Update(double load, bool slow)
	{
		if (m_efficiency)
		{
			if (m_load_before_move > 0.02 && load > 0.02)
				m_slowdown = std::clamp(Average(m_slowdown, load / m_load_before_move), 1.2, 6.0);
			if (++m_windows >= SettledWindows)
				m_hold_time = HoldWindows;
			if (load > PromoteLoad || slow)
			{
				// Back soon after the last move: hold longer this time.
				if (m_windows < SettledWindows && m_moved_before)
					m_hold_time = std::min(m_hold_time * 2, MaxHoldWindows);
				m_moved_before = true;
				m_efficiency = false;
				m_hold = m_hold_time;
				m_calm = 0;
				m_windows = 0;
				m_average = load / m_slowdown;
			}
			return m_efficiency;
		}
		m_average = m_windows++ ? Average(m_average, load) : load;
		if (m_hold)
		{
			m_hold--;
			return m_efficiency;
		}
		if (slow || m_average * m_slowdown >= DemoteLoad)
			m_calm = 0;
		else if (++m_calm >= CalmWindows)
		{
			m_load_before_move = m_average;
			m_efficiency = true;
			m_calm = 0;
			m_windows = 0;
		}
		return m_efficiency;
	}

private:
	static double Average(double average, double sample) { return average + (sample - average) * 0.3; }

	bool m_efficiency = false;
	bool m_moved_before = false;
	unsigned m_calm = 0;
	unsigned m_hold = 0;
	unsigned m_hold_time = HoldWindows;
	unsigned m_windows = 0;
	double m_average = 0.0; // load on the performance cores
	double m_slowdown = 2.5; // efficiency-core time over performance-core time for the same work
	double m_load_before_move = 0.0;
};
