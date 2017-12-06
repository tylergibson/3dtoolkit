#include "pch.h"

#include <stdlib.h>
#include <shellapi.h>
#include <fstream>
#include <atomic>

#include "DeviceResources.h"
#include "CubeRenderer.h"
#include "macros.h"

#ifdef TEST_RUNNER
#include "test_runner.h"
#else // TEST_RUNNER
#include "server_main_window.h"
#include "server_authentication_provider.h"
#include "turn_credential_provider.h"
#include "server_renderer.h"
#include "webrtc.h"
#include "config_parser.h"
#include "directx_buffer_capturer.h"
#include "service/render_service.h"
#endif // TEST_RUNNER

// Required app libs
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "usp10.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "winmm.lib")

#ifndef TEST_RUNNER
using namespace Microsoft::WRL;
#endif // TEST_RUNNER

using namespace DX;
using namespace StreamingToolkit;
using namespace StreamingToolkitSample;

//--------------------------------------------------------------------------------------
// Forward declarations
//--------------------------------------------------------------------------------------
void StartRenderService();

//--------------------------------------------------------------------------------------
// Global Variables
//--------------------------------------------------------------------------------------
HWND				g_hWnd = nullptr;
DeviceResources*	g_deviceResources = nullptr;
CubeRenderer*		g_cubeRenderer = nullptr;
#ifdef TEST_RUNNER
VideoTestRunner*	g_videoTestRunner = nullptr;
#endif // TESTRUNNER

#ifndef TEST_RUNNER


struct SingleInstanceViewData
{
	// dirextx vectors must be 16byte aligned
	// so we override the operators to support
	// new-ing this class

	void* operator new(size_t i)
	{
		return _mm_malloc(i, 16);
	}

	void operator delete(void* p)
	{
		_mm_free(p);
	}

	bool IsZeroed()
	{
		return DirectX::XMVector3Equal(lookAt, DirectX::XMVectorZero()) &&
			DirectX::XMVector3Equal(up, DirectX::XMVectorZero()) &&
			DirectX::XMVector3Equal(eye, DirectX::XMVectorZero());
	}

	DirectX::XMVECTORF32 lookAt;
	DirectX::XMVECTORF32 up;
	DirectX::XMVECTORF32 eye;
};

struct SingleInstance
{
	std::atomic_bool shouldProcessEvents;
	rtc::scoped_refptr<Conductor> conductor;
	PeerConnectionClient* client;
	DirectXBufferCapturer* bufferCapturer;
	std::shared_ptr<SingleInstanceViewData> mostRecentView;
};

void CreateSingleInstanceThread(const std::string& instanceName,
	DeviceResources* deviceResources,
	MainWindow* wnd,
	std::vector<std::shared_ptr<SingleInstance>>& instanceList)
{
	rtc::Event allocationCompleted(true, false);

	// TODO(bengreenier): are we cool new-ing this into oblivion?
	new std::thread([&]
	{
		rtc::Win32Thread threadWrapper;
		rtc::ThreadManager::Instance()->SetCurrentThread(&threadWrapper);

		std::shared_ptr<SingleInstanceViewData> mostRecentView(new SingleInstanceViewData());

		InputDataHandler handler([&](const std::string& message)
		{
			char type[256];
			char body[1024];
			Json::Reader reader;
			Json::Value msg = NULL;
			reader.parse(message, msg, false);

			if (msg.isMember("type") && msg.isMember("body"))
			{
				strcpy(type, msg.get("type", "").asCString());
				strcpy(body, msg.get("body", "").asCString());
				std::istringstream datastream(body);
				std::string token;

				if (strcmp(type, "camera-transform-lookat") == 0)
				{
					// Eye point.
					getline(datastream, token, ',');
					float eyeX = stof(token);
					getline(datastream, token, ',');
					float eyeY = stof(token);
					getline(datastream, token, ',');
					float eyeZ = stof(token);

					// Focus point.
					getline(datastream, token, ',');
					float focusX = stof(token);
					getline(datastream, token, ',');
					float focusY = stof(token);
					getline(datastream, token, ',');
					float focusZ = stof(token);

					// Up vector.
					getline(datastream, token, ',');
					float upX = stof(token);
					getline(datastream, token, ',');
					float upY = stof(token);
					getline(datastream, token, ',');
					float upZ = stof(token);

					const DirectX::XMVECTORF32 lookAt = { focusX, focusY, focusZ, 0.f };
					const DirectX::XMVECTORF32 up = { upX, upY, upZ, 0.f };
					const DirectX::XMVECTORF32 eye = { eyeX, eyeY, eyeZ, 0.f };

					mostRecentView->lookAt = lookAt;
					mostRecentView->up = up;
					mostRecentView->eye = eye;
				}
			}
		});

		PeerConnectionClient* client = new PeerConnectionClient(instanceName);

		// Note: Conductor is responsible for cleaning up bufferCapturer object.
		DirectXBufferCapturer* bufferCapturer = new DirectXBufferCapturer(
			deviceResources->GetD3DDevice());

		bufferCapturer->Initialize();

		auto webrtcConfig = GlobalObject<WebRTCConfig>::Get();

		rtc::scoped_refptr<Conductor> conductor(new rtc::RefCountedObject<Conductor>(
			client, bufferCapturer, wnd, webrtcConfig.get()));

		conductor->SetInputDataHandler(&handler);
		client->SetHeartbeatMs(webrtcConfig->heartbeat);

		std::shared_ptr<SingleInstance> instance(new SingleInstance());
		
		instance->shouldProcessEvents.store(true);
		instance->conductor = conductor;
		instance->client = client;
		instance->bufferCapturer = bufferCapturer;
		instance->mostRecentView = mostRecentView;

		instanceList.push_back(instance);

		// inform the parent thread that allocation is complete
		allocationCompleted.Set();

		// process until stopped
		while (instance->shouldProcessEvents.load())
		{
			threadWrapper.ProcessMessages(500);
		}
	});

	// wait until allocation of this thread is complete before returning
	allocationCompleted.Wait(rtc::Event::kForever);
}

bool AppMain(BOOL stopping)
{
	auto webrtcConfig = GlobalObject<WebRTCConfig>::Get();
	auto serverConfig = GlobalObject<ServerConfig>::Get();

	ServerAuthenticationProvider::ServerAuthInfo authInfo;
	authInfo.authority = webrtcConfig->authentication.authority;
	authInfo.resource = webrtcConfig->authentication.resource;
	authInfo.clientId = webrtcConfig->authentication.client_id;
	authInfo.clientSecret = webrtcConfig->authentication.client_secret;

	rtc::EnsureWinsockInit();
	rtc::Win32Thread w32_thread;
	rtc::ThreadManager::Instance()->SetCurrentThread(&w32_thread);

	ServerMainWindow wnd(
		webrtcConfig->server.c_str(),
		webrtcConfig->port,
		FLAG_autoconnect,
		FLAG_autocall,
		false,
		serverConfig->server_config.width,
		serverConfig->server_config.height);

	if (!serverConfig->server_config.system_service && !wnd.Create())
	{
		RTC_NOTREACHED();
		return -1;
	}

	// Initializes the device resources.
	g_deviceResources = new DeviceResources();
	g_deviceResources->SetWindow(wnd.handle());

	// Initializes the cube renderer.
	g_cubeRenderer = new CubeRenderer(g_deviceResources);

	rtc::InitializeSSL();

	// Gets the frame buffer from the swap chain.
	ComPtr<ID3D11Texture2D> frameBuffer;
	if (!serverConfig->server_config.system_service)
	{
		HRESULT hr = g_deviceResources->GetSwapChain()->GetBuffer(
			0,
			__uuidof(ID3D11Texture2D),
			reinterpret_cast<void**>(frameBuffer.GetAddressOf()));
	}

	std::vector<std::shared_ptr<SingleInstance>> instances;

	// allocate threads
	for (auto i = 0; i < 1; ++i)
	{
		// create an instance, adding itself to instances and handles
		// note: this is blocking until it's been added to the lists
		CreateSingleInstanceThread(
			"[" + std::to_string(i) + "]",
			g_deviceResources,
			&wnd,
			instances
		);
	}

	// reset the current thread in case CSIT changed it
	// TODO(bengreenier): do we need to do this?
	rtc::ThreadManager::Instance()->SetCurrentThread(&w32_thread);

	// Phong Cao: TODO - Parses from config file.
	int targetFps = 60;
	int interval = 1000 / targetFps;

	// Main loop.
	while (!stopping)
	{
		MSG msg = { 0 };

		// For system service, ignore window and swap chain.
		if (serverConfig->server_config.system_service)
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{
				if (!wnd.PreTranslateMessage(&msg))
				{
					TranslateMessage(&msg);
					DispatchMessage(&msg);
				}
			}
			else
			{
				if (std::any_of(instances.begin(), instances.end(), [](std::shared_ptr<SingleInstance> i)
				{
					return i->conductor->is_closing();
				}))
				{
					break;
				}

				if (std::any_of(instances.begin(), instances.end(), [](std::shared_ptr<SingleInstance> i)
				{
					// TODO(bengreenier): do we need this second condition?
					return i->conductor->connection_active() || i->client->is_connected();
				}))
				{
					ULONGLONG tick = GetTickCount64();
					g_cubeRenderer->Update();

					for each (auto instance in instances)
					{
						auto mrv = instance->mostRecentView;
						auto bc = instance->bufferCapturer;

						if (!mrv->IsZeroed())
						{
							g_cubeRenderer->UpdateView(mrv->eye, mrv->lookAt, mrv->up);
						}
						g_cubeRenderer->Render();

						bc->SendFrame(frameBuffer.Get());
					}

					// TODO(bengreenier): by doing just a present here, we will always see
					// on our screen the render from the view of the last instance in the list
					g_deviceResources->Present();

					// FPS limiter.
					ULONGLONG timeElapsed = GetTickCount64() - tick;
					DWORD sleepAmount = 0;
					if (timeElapsed < interval)
					{
						sleepAmount = interval - timeElapsed;
					}

					Sleep(sleepAmount);
				}
			}
		}
	}

	// shutdown the instances
	for each (auto instance in instances)
	{
		instance->shouldProcessEvents.store(false);

		// TODO(bengreenier): we should add a "wait" here until the instance thread is complete

		instance->conductor->Release();
		delete instance->client;
	}

	rtc::CleanupSSL();

	// Cleanup.
	delete g_cubeRenderer;
	delete g_deviceResources;

	return 0;
}

//--------------------------------------------------------------------------------------
// System service
//--------------------------------------------------------------------------------------
void StartRenderService()
{
	SC_HANDLE schSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
	if (schSCManager)
	{
		// Init service's main function.
		const std::function<void(BOOL*)> serviceMainFunc = [&](BOOL* stopping)
		{
			AppMain(*stopping);
		};

		auto serverConfig = GlobalObject<ServerConfig>::Get();

		RenderService service((PWSTR)serverConfig->service_config.name.c_str(), serviceMainFunc);

		// Starts the service to run the app persistently.
		if (!CServiceBase::Run(service))
		{
			wprintf(L"Service failed to run w/err 0x%08lx\n", GetLastError());
			MessageBox(
				NULL,
				L"Service needs to be initialized using PowerShell scripts.",
				L"Error",
				MB_ICONERROR
			);
		}

		CloseServiceHandle(schSCManager);
		schSCManager = NULL;
	}
}

#else // TEST_RUNNER

//--------------------------------------------------------------------------------------
// Called every time the application receives a message
//--------------------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	PAINTSTRUCT ps;
	HDC hdc;

	switch (message)
	{
	case WM_PAINT:
		hdc = BeginPaint(hWnd, &ps);
		EndPaint(hWnd, &ps);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

		// Note that this tutorial does not handle resizing (WM_SIZE) requests,
		// so we created the window without the resize border.

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}

	return 0;
}

//--------------------------------------------------------------------------------------
// Registers class and creates window
//--------------------------------------------------------------------------------------
HRESULT InitWindow(HINSTANCE hInstance, int nCmdShow)
{
	// Registers class.
	WNDCLASSEX wcex = { 0 };
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = hInstance;
	wcex.hIcon = 0;
	wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = nullptr;
	wcex.lpszClassName = L"SpinningCubeClass";
	if (!RegisterClassEx(&wcex))
	{
		return E_FAIL;
	}

	// Creates window.
	RECT rc = { 0, 0, 1280, 720 };
	AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
	g_hWnd = CreateWindow(
		L"SpinningCubeClass",
		L"SpinningCube",
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		rc.right - rc.left,
		rc.bottom - rc.top,
		nullptr,
		nullptr,
		hInstance,
		nullptr);

	if (!g_hWnd)
	{
		return E_FAIL;
	}

	ShowWindow(g_hWnd, nCmdShow);

	return S_OK;
}

//--------------------------------------------------------------------------------------
// Render the frame
//--------------------------------------------------------------------------------------
void Render()
{
	g_cubeRenderer->Update();
	g_cubeRenderer->Render();
	g_deviceResources->Present();
}

#endif // TEST_RUNNER

//--------------------------------------------------------------------------------------
// Entry point to the program. Initializes everything and goes into a message processing 
// loop. Idle time is used to render the scene.
//--------------------------------------------------------------------------------------
int WINAPI wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

#ifdef TEST_RUNNER
	if (FAILED(InitWindow(hInstance, nCmdShow)))
	{
		return 0;
	}

	// Initializes the device resources.
	g_deviceResources = new DeviceResources();
	g_deviceResources->SetWindow(g_hWnd);

	// Initializes the cube renderer.
	g_cubeRenderer = new CubeRenderer(g_deviceResources);

	RECT rc;
	GetClientRect(g_hWnd, &rc);
	UINT width = rc.right - rc.left;
	UINT height = rc.bottom - rc.top;

	// Creates and initializes the video test runner library.
	g_videoTestRunner = new VideoTestRunner(
		g_deviceResources->GetD3DDevice(),
		g_deviceResources->GetD3DDeviceContext()); 

	g_videoTestRunner->StartTestRunner(g_deviceResources->GetSwapChain());
	
	// Main message loop.
	MSG msg = { 0 };
	while (WM_QUIT != msg.message)
	{
		if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		else
		{
			Render();

			if (g_videoTestRunner->TestsComplete())
			{
				break;
			}

			g_videoTestRunner->TestCapture();
			if (g_videoTestRunner->IsNewTest()) 
			{
				delete g_cubeRenderer;
				g_cubeRenderer = new CubeRenderer(g_deviceResources);
			}
		}
	}

	delete g_cubeRenderer;
	delete g_deviceResources;

	return (int)msg.wParam;
#else // TEST_RUNNER
	
	// setup the config parsers
	ConfigParser::ConfigureConfigFactories();

	auto serverConfig = GlobalObject<ServerConfig>::Get();

	if (!serverConfig->server_config.system_service)
	{
		return AppMain(FALSE);
	}
	else
	{
		StartRenderService();
		return 0;
	}
#endif // TEST_RUNNER
}
