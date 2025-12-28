#pragma once
#include <iostream>
#include <chrono>

class Timer 
{
public:
    Timer() : m_bRunning(false) {}
    Timer(const Timer& timer)
    {
        m_bRunning = timer.m_bRunning;
        m_end_time = timer.m_end_time;
        m_start_time = timer.m_start_time;
    }
    void start() 
    {
        m_start_time = std::chrono::high_resolution_clock::now();
        m_bRunning = true;
    }

    void end() 
    {
        if(m_bRunning) 
        {
            m_end_time = std::chrono::high_resolution_clock::now();
            m_bRunning = false;
        }
    }

    double seconds() 
    {
        if(m_bRunning) 
            m_end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = m_end_time - m_start_time;
        return elapsed.count();
    }
private:
    std::chrono::high_resolution_clock::time_point m_start_time;
    std::chrono::high_resolution_clock::time_point m_end_time;
    bool m_bRunning;
};