#include "pch.h"

#include <fstream>

#include "buffer_capturer.h"

namespace StreamingToolkit
{
	BufferCapturer::BufferCapturer() :
		clock_(webrtc::Clock::GetRealTimeClock()),
		running_(false),
		sink_(nullptr),
		use_software_encoder_(false),
		sink_wants_observer_(nullptr)
	{
		SetCaptureFormat(NULL);
		set_enable_video_adapter(false);
	}

	cricket::CaptureState BufferCapturer::Start(const cricket::VideoFormat& format)
	{
		SetCaptureFormat(&format);

		// Phong Cao: TODO - Creates nvEncConfig which parses json file.
		use_software_encoder_ = false;

		running_ = true;
		SetCaptureState(cricket::CS_RUNNING);
		return cricket::CS_RUNNING;
	}

	void BufferCapturer::Stop()
	{
		rtc::CritScope cs(&lock_);
		running_ = false;
	}

	void BufferCapturer::SetSinkWantsObserver(SinkWantsObserver* observer)
	{
		rtc::CritScope cs(&lock_);
		RTC_DCHECK(!sink_wants_observer_);
		sink_wants_observer_ = observer;
	}

	void BufferCapturer::AddOrUpdateSink(
		rtc::VideoSinkInterface<VideoFrame>* sink,
		const rtc::VideoSinkWants& wants) 
	{
		rtc::CritScope cs(&lock_);
		sink_ = sink;
		if (sink_wants_observer_)
		{
			sink_wants_observer_->OnSinkWantsChanged(sink, wants);
		}
	}

	bool BufferCapturer::IsRunning() 
	{
		return running_;
	}

	bool BufferCapturer::IsScreencast() const 
	{
		return false;
	}

	bool BufferCapturer::GetPreferredFourccs(std::vector<uint32_t>* fourccs)
	{
		fourccs->push_back(cricket::FOURCC_H264);
		return true;
	}

	void BufferCapturer::RemoveSink(rtc::VideoSinkInterface<VideoFrame>* sink) 
	{
		rtc::CritScope cs(&lock_);
		sink_ = nullptr;
	}
};
