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
	d3d_device_->GetImmediateContext(&d3d_context_);

#ifdef MULTITHREAD_PROTECTION
	// Enables multithread protection.
	ID3D11Multithread* multithread;
	d3d_device_->QueryInterface(IID_PPV_ARGS(&multithread));
	multithread->SetMultithreadProtected(true);
	multithread->Release();
#endif // MULTITHREAD_PROTECTION

	// Initializes NVIDIA encoder.
	webrtc::H264EncoderImpl::SetDevice(d3d_device_.Get());
	webrtc::H264EncoderImpl::SetContext(d3d_context_.Get());
}

void DirectXBufferCapturer::SendFrame(webrtc::VideoFrame video_frame)
{
	if (!running_)
	{
		return;
	}

	if (sink_)
	{
		sink_->OnFrame(video_frame);
	}
	else
	{
		OnFrame(video_frame, video_frame.width(), video_frame.height());
	}
}

void DirectXBufferCapturer::SendFrame(ID3D11Texture2D* frame_buffer)
{
	if (!running_)
	{
		return;
	}

	D3D11_TEXTURE2D_DESC desc;
	frame_buffer->GetDesc(&desc);
	UpdateStagingBuffer(frame_buffer);

	rtc::scoped_refptr<webrtc::I420Buffer> buffer;
	buffer = webrtc::I420Buffer::Create(desc.Width, desc.Height);
	auto time_stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();

	auto frame = webrtc::VideoFrame(buffer, kVideoRotation_0, 0);
	staging_frame_buffer_.Get()->AddRef();
	frame.SetID3D11Texture2D(staging_frame_buffer_.Get());
	frame.set_ntp_time_ms(clock_->CurrentNtpInMilliseconds());
	frame.set_rotation(VideoRotation::kVideoRotation_0);
	SendFrame(frame);
}

void DirectXBufferCapturer::UpdateStagingBuffer(ID3D11Texture2D* frame_buffer)
{
	D3D11_TEXTURE2D_DESC desc;
	frame_buffer->GetDesc(&desc);

	// Lazily initializes the staging frame buffer.
	if (!staging_frame_buffer_)
	{
		staging_frame_buffer_desc_ = { 0 };
		staging_frame_buffer_desc_.ArraySize = 1;
		staging_frame_buffer_desc_.Format = desc.Format;
		staging_frame_buffer_desc_.Width = desc.Width;
		staging_frame_buffer_desc_.Height = desc.Height;
		staging_frame_buffer_desc_.MipLevels = 1;
		staging_frame_buffer_desc_.SampleDesc.Count = 1;
		staging_frame_buffer_desc_.Usage = D3D11_USAGE_DEFAULT;
		d3d_device_->CreateTexture2D(
			&staging_frame_buffer_desc_, nullptr, &staging_frame_buffer_);
	}
	// Resizes if needed.
	else if (staging_frame_buffer_desc_.Width != desc.Width || 
		staging_frame_buffer_desc_.Height != desc.Height)
	{
		staging_frame_buffer_desc_.Width = desc.Width;
		staging_frame_buffer_desc_.Height = desc.Height;
		d3d_device_->CreateTexture2D(&staging_frame_buffer_desc_, nullptr,
			&staging_frame_buffer_);
	}

	// Copies the frame buffer to the staging one.		
	d3d_context_->CopyResource(staging_frame_buffer_.Get(), frame_buffer);
}
