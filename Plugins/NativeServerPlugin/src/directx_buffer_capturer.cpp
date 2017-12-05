#include "pch.h"

#include "directx_buffer_capturer.h"
#include "plugindefs.h"

#include "webrtc/modules/video_coding/codecs/h264/h264_encoder_impl.h"

using namespace Microsoft::WRL;
using namespace StreamingToolkit;

DirectXBufferCapturer::DirectXBufferCapturer(ID3D11Device* d3d_device) :
	d3d_device_(d3d_device)
{
}

void DirectXBufferCapturer::Initialize()
{
	// Gets the device context.
	ComPtr<ID3D11DeviceContext> device_context;
	d3d_device_->GetImmediateContext(&device_context);

#ifdef MULTITHREAD_PROTECTION
	// Enables multithread protection.
	ID3D11Multithread* multithread;
	d3d_device_->QueryInterface(IID_PPV_ARGS(&multithread));
	multithread->SetMultithreadProtected(true);
	multithread->Release();
#endif // MULTITHREAD_PROTECTION

	// Initializes NVIDIA encoder.
	webrtc::H264EncoderImpl::SetDevice(d3d_device_.Get());
	webrtc::H264EncoderImpl::SetContext(device_context.Get());
}

void DirectXBufferCapturer::SendFrame(webrtc::VideoFrame frame)
{
	rtc::CritScope cs(&lock_);
	if (running_)
	{
		// Phong Cao: TODO
		OutputDebugStringA("SendFrame!!!\n");
		return;

		if (sink_)
		{
			sink_->OnFrame(frame);
		}
		else
		{
			OnFrame(frame, frame.width(), frame.height());
		}
	}
}

void DirectXBufferCapturer::SendFrame(ID3D11Texture2D* texture)
{
	rtc::scoped_refptr<webrtc::I420Buffer> buffer;
	auto time_stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();

	auto frame = webrtc::VideoFrame(buffer, kVideoRotation_0, 0);
	SendFrame(frame);
}
