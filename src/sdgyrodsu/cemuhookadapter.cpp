#include "cemuhookadapter.h"
#include "sdhidframe.h"
#include "log.h"

#include <iostream>

using namespace kmicki::cemuhook::protocol;
using namespace kmicki::log;

#define SD_SCANTIME_US 4000
#define ACC_1G 0x4000
#define GYRO_1DEGPERSEC 16
#define GYRO_DEADZONE 8
#define ACCEL_SMOOTH 0x1FF

namespace kmicki::sdgyrodsu
{

    MotionData CemuhookAdapter::GetMotionData(SdHidFrame const& frame, float &lastAccelRtL, float &lastAccelFtB, float &lastAccelTtB)
    {
        MotionData data;

        SetMotionData(frame,data,lastAccelRtL,lastAccelFtB,lastAccelTtB);

        return data;
    }
    
    float SmoothAccel(float &last, int16_t curr)
    {
        static const float acc1G = (float)ACC_1G;
        if(abs(curr - last) < ACCEL_SMOOTH)
        {
            last = ((float)last*0.95+(float)curr*0.05);
        }
        else
        {
            last = (float)curr;
        }
        return last/acc1G;
    }

    MotionData & SetTimestamp(MotionData &data, uint64_t const& timestamp)
    {
        data.timestampL = (uint32_t)(timestamp & 0xFFFFFFFF);
        data.timestampH = (uint32_t)(timestamp >> 32);

        return data;
    }

    uint64_t ToTimestamp(uint32_t const& increment)
    {
        return (uint64_t)increment*SD_SCANTIME_US;
    }

    MotionData &  SetTimestamp(MotionData &data, uint32_t const& increment)
    {
        SetTimestamp(data,ToTimestamp(increment));

        return data;
    }

    void CemuhookAdapter::SetMotionData(SdHidFrame const& frame, MotionData &data, float &lastAccelRtL, float &lastAccelFtB, float &lastAccelTtB)
    {
        static const float acc1G = (float)ACC_1G;
        static const float gyro1dps = (float)GYRO_1DEGPERSEC;

        SetTimestamp(data, frame.Increment);
        
        data.accX = -SmoothAccel(lastAccelRtL,frame.AccelAxisRightToLeft);
        data.accY = -SmoothAccel(lastAccelFtB,frame.AccelAxisFrontToBack);
        data.accZ = SmoothAccel(lastAccelTtB,frame.AccelAxisTopToBottom);
        if(frame.Header & 0xFF == 0xDD)
        {
            data.pitch = 0.0f;
            data.yaw = 0.0f;
            data.roll = 0.0f;
        }
        else 
        {
            auto gyroRtL = frame.GyroAxisRightToLeft;
            auto gyroFtB = frame.GyroAxisFrontToBack;
            auto gyroTtB = frame.GyroAxisTopToBottom;

            if(gyroRtL < GYRO_DEADZONE && gyroRtL > -GYRO_DEADZONE)
                gyroRtL = 0;
            if(gyroFtB < GYRO_DEADZONE && gyroFtB > -GYRO_DEADZONE)
                gyroFtB = 0;
            if(gyroTtB < GYRO_DEADZONE && gyroTtB > -GYRO_DEADZONE)
                gyroTtB = 0;

            data.pitch = (float)gyroRtL/gyro1dps;
            data.yaw = -(float)gyroFtB/gyro1dps;
            data.roll = (float)gyroTtB/gyro1dps;
        }
    }

    CemuhookAdapter::CemuhookAdapter(hiddev::HidDevReader & _reader, bool persistent)
    : reader(_reader),
      lastInc(0),
      lastAccelRtL(0.0),lastAccelFtB(0.0),lastAccelTtB(0.0),
      isPersistent(persistent), toReplicate(0)
    {
        Log("CemuhookAdapter: Initialized. Waiting for start of frame grab.");
    }

    void CemuhookAdapter::StartFrameGrab()
    {
        lastInc = 0;
        Log("CemuhookAdapter: Starting frame grab.");
        reader.Start();
    }

    int const& CemuhookAdapter::SetMotionDataNewFrame(MotionData &motion)
    {
        static const int64_t cMaxDiffReplicate = 1000;

        while(true)
        {
            if(toReplicate == 0)
            {
                auto const& frame = GetSdFrame(reader.GetNewFrame(this));
                int64_t diff = frame.Increment - lastInc;

                if(lastInc != 0 && diff < 1 && diff > -100)
                {
                    reader.UnlockFrame(this);
                    Log("CemuhookAdapter: Frame was repeated. Ignoring...");
                }
                else
                {
                    if(lastInc != 0 && diff > 1)
                    {
                        { LogF msg; msg << "CemuhookAdapter: Missed " << (diff-1) << " frames."; }
                        if(diff <= cMaxDiffReplicate)
                        {
                            Log("CemuhookAdapter: Replicating frames...");
                            toReplicate = diff-1;
                        }
                    }

                    SetMotionData(frame,motion,lastAccelRtL,lastAccelFtB,lastAccelTtB);
                    reader.UnlockFrame(this);

                    if(toReplicate > 0)
                    {
                        lastTimestamp = ToTimestamp(lastInc+1);
                        SetTimestamp(motion,lastTimestamp);
                        if(!isPersistent)
                            data = motion;
                    }
                        
                    lastInc = frame.Increment;
                    
                    return toReplicate;
                }
            }
            else
            {
                // Replicated frame
                --toReplicate;
                lastTimestamp += SD_SCANTIME_US;
                if(!isPersistent)
                {
                    motion = SetTimestamp(data,lastTimestamp);
                }
                else
                    SetTimestamp(motion,lastTimestamp);

                return toReplicate;
            }
        }
    }

    void CemuhookAdapter::StopFrameGrab()
    {
        Log("CemuhookAdapter: Stopping frame grab.");
        reader.Stop();
    }

    bool CemuhookAdapter::IsControllerConnected()
    {
        return true;
    }
}