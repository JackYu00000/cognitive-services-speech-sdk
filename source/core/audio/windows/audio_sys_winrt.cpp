//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.
//  audio_sys_winrt.cpp implements audio_sys interface for Windows Store.
//

#include <stdlib.h>
#include <stdint.h>
#include <mutex>
#include <wrl/implements.h>
#include "audio_sys.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/buffer_.h"
#include <windows.h>
#include <mmsystem.h>
#include <memory>
#include <future>

#include <initguid.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmstream.h>
#include <endpointvolume.h>
#include <spxdebug.h>
#include <windows/audio_sys_win_base.h>

using namespace Microsoft::WRL;

const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);

class WASAPICapture :
    public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler>
{
public:
    WASAPICapture();
    virtual ~WASAPICapture();

    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation *operation);

public:
    ComPtr<IAudioClient> pAudioInputClient;
    WAVEFORMATEX audioInFormat;
    HANDLE hBufferReady;
    HANDLE hCaptureThreadShouldExit;

    std::promise<HRESULT> m_promise;
};

typedef struct AUDIO_SYS_DATA_TAG
{
    //Audio Input Context
    std::thread hCaptureThread;
    ComPtr<WASAPICapture> spCapture;

    //Audio Output Context
    IMMDevice * pAudioOutputDevice;
    HANDLE hRenderThreadShouldExit;
    HANDLE hRenderThreadDidExit;
    BOOL output_canceled;

    ON_AUDIOERROR_CALLBACK error_cb;
    ON_AUDIOOUTPUT_STATE_CALLBACK output_state_cb;
    ON_AUDIOINPUT_STATE_CALLBACK input_state_cb;
    AUDIOINPUT_WRITE audio_write_cb;
    void * user_write_ctx;
    void * user_outputctx;
    void * user_inputctx;
    void * user_errorctx;
    AUDIO_STATE current_output_state;
    AUDIO_STATE current_input_state;
    STRING_HANDLE hDeviceName;
    size_t inputFrameCnt;
} AUDIO_SYS_DATA;


//-------------------
// helpers
HRESULT audio_create_events(AUDIO_SYS_DATA * const audioData);

WASAPICapture::WASAPICapture()
{
    HRESULT hr = S_OK;

    hBufferReady = CreateEvent(0, FALSE, FALSE, nullptr);
    EXIT_ON_ERROR_IF(E_FAIL, nullptr == hBufferReady);

    hCaptureThreadShouldExit = CreateEvent(0, FALSE, FALSE, nullptr);
    EXIT_ON_ERROR_IF(E_FAIL, nullptr == hCaptureThreadShouldExit);

Exit:
    if (FAILED(hr))
    {
        SAFE_CLOSE_HANDLE(hBufferReady);
        SAFE_CLOSE_HANDLE(hCaptureThreadShouldExit);
    }
}

WASAPICapture::~WASAPICapture()
{
    if(INVALID_HANDLE_VALUE != hBufferReady)
    {
        SAFE_CLOSE_HANDLE(hBufferReady);
    }
    if (INVALID_HANDLE_VALUE != hCaptureThreadShouldExit)
    {
        SAFE_CLOSE_HANDLE(hCaptureThreadShouldExit);
    }
}
HRESULT WASAPICapture::ActivateCompleted(IActivateAudioInterfaceAsyncOperation *operation)
{
    HRESULT hrActivateResult = S_OK;
    ComPtr<IUnknown> punkAudioInterface;
    REFERENCE_TIME hnsRequestedDuration = REFTIMES_PER_SEC;

    // Check for a successful activation result
    HRESULT hr = operation->GetActivateResult(&hrActivateResult, &punkAudioInterface);
    EXIT_ON_ERROR(hr);

    EXIT_ON_ERROR(hrActivateResult);

    // Get the pointer for the Audio Client
    punkAudioInterface.CopyTo(&pAudioInputClient);
    EXIT_ON_ERROR_IF(E_FAIL, nullptr == pAudioInputClient);

    audioInFormat.wFormatTag = AUDIO_FORMAT_PCM;
    audioInFormat.wBitsPerSample = AUDIO_BITS;
    audioInFormat.nChannels = AUDIO_CHANNELS_MONO;
    audioInFormat.nSamplesPerSec = AUDIO_SAMPLE_RATE;
    audioInFormat.nAvgBytesPerSec = AUDIO_BYTE_RATE;
    audioInFormat.nBlockAlign = AUDIO_BLOCK_ALIGN;
    audioInFormat.cbSize = 0;

    // the rest of the code assume 1 frame has two bytes.
    // see numFramesAvailable * 2
    static_assert(AUDIO_BLOCK_ALIGN == 2, "Sure one frame has two bytes.");

    hr = pAudioInputClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
        hnsRequestedDuration,
        0,
        &audioInFormat,
        nullptr);
    EXIT_ON_ERROR(hr);

    hr = pAudioInputClient->SetEventHandle(hBufferReady);

Exit:
    if (FAILED(hr))
    {
        pAudioInputClient->Release();
    }
    m_promise.set_value(hr);

    // Need to return S_OK
    return S_OK;
}

HRESULT audio_input_create(AUDIO_SYS_DATA* result)
{
    ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
    HRESULT hr = S_OK;

    PWSTR audioCaptureGuidString;
    StringFromIID(DEVINTERFACE_AUDIO_CAPTURE, &audioCaptureGuidString);

    result->spCapture = Make<WASAPICapture>();
    auto future = result->spCapture->m_promise.get_future();
    // This call must be made on the main UI thread.  Async operation will call back to
    // IActivateAudioInterfaceCompletionHandler::ActivateCompleted, which must be an agile interface implementation
    hr = ActivateAudioInterfaceAsync(audioCaptureGuidString, __uuidof(IAudioClient2), nullptr, result->spCapture.Get(), &asyncOp);
    EXIT_ON_ERROR(hr);

    // wait
    future.wait();
    hr = future.get();

Exit:
    CoTaskMemFree(audioCaptureGuidString);
    return hr;
}

AUDIO_SYS_HANDLE audio_create()
{
    HRESULT hr = S_OK;
    AUDIO_SYS_DATA * result = nullptr;

    result = new AUDIO_SYS_DATA();

    memset(result, 0, sizeof(AUDIO_SYS_DATA));

    result->current_output_state = AUDIO_STATE_STOPPED;
    result->current_input_state = AUDIO_STATE_STOPPED;
    // set input frame to 10 ms.(16000 frames(samples) is in one second )
    result->inputFrameCnt = 160;

    hr = audio_input_create(result);
    EXIT_ON_ERROR(hr);

Exit:
    if (FAILED(hr))
    {
        delete result;
        result = nullptr;
    }
    return result;
}

HRESULT audio_create_events(AUDIO_SYS_DATA * const audioData)
{
    HRESULT hr = S_OK;
    if (audioData == nullptr)
    {
        return AUDIO_RESULT_INVALID_ARG;
    }

    audioData->hRenderThreadShouldExit = CreateEvent(0, FALSE, FALSE, nullptr);
    EXIT_ON_ERROR_IF(E_FAIL, nullptr == audioData->hRenderThreadShouldExit);

    // n.b. starts signaled so force_render_thread_to_exit_and_wait will work even if audio was never played
    audioData->hRenderThreadDidExit = CreateEvent(0, FALSE, TRUE, nullptr);
    EXIT_ON_ERROR_IF(E_FAIL, NULL == audioData->hRenderThreadDidExit);

Exit:
    if (FAILED(hr))
    {

        SAFE_CLOSE_HANDLE(audioData->hRenderThreadShouldExit);
        SAFE_CLOSE_HANDLE(audioData->hRenderThreadDidExit);

    }
    return hr;
}

// Returns 1 on success.
// Note: There are so many conflicting or competing conditions here (and not just here!) since we
// don't lock the audio structure at all. If we experience real problems we
// should go back and add proper locking.
static void force_render_thread_to_exit_and_wait(AUDIO_SYS_DATA *audioData)
{
    audioData->output_canceled = TRUE;
    SetEvent(audioData->hRenderThreadShouldExit);
    WaitForSingleObject(audioData->hRenderThreadDidExit, INFINITE);
}

void audio_destroy(AUDIO_SYS_HANDLE handle)
{
    if (handle != NULL)
    {
        AUDIO_SYS_DATA* audioData = (AUDIO_SYS_DATA*)handle;

        audioData->spCapture->pAudioInputClient->Release();
        if (audioData->hRenderThreadShouldExit != NULL  &&
            audioData->hRenderThreadDidExit != NULL)
        {
            force_render_thread_to_exit_and_wait(audioData);
        }
        SAFE_CLOSE_HANDLE(audioData->hRenderThreadShouldExit);
        SAFE_CLOSE_HANDLE(audioData->hRenderThreadDidExit);

        if (audioData->hDeviceName)
        {
            STRING_delete(audioData->hDeviceName);
        }

        delete audioData;
    }
}

AUDIO_RESULT audio_setcallbacks(AUDIO_SYS_HANDLE              handle,
                                ON_AUDIOOUTPUT_STATE_CALLBACK output_cb,
                                void*                         output_ctx,
                                ON_AUDIOINPUT_STATE_CALLBACK  input_cb,
                                void*                         input_ctx,
                                AUDIOINPUT_WRITE              audio_write_cb,
                                void*                         audio_write_ctx,
                                ON_AUDIOERROR_CALLBACK        error_cb,
                                void*                         error_ctx)
{
    AUDIO_RESULT result = AUDIO_RESULT_INVALID_ARG;
    if (handle != NULL && audio_write_cb != NULL)
    {
        AUDIO_SYS_DATA* audioData = (AUDIO_SYS_DATA*)handle;

        audioData->error_cb = error_cb;
        audioData->user_errorctx = error_ctx;
        audioData->user_errorctx = error_cb;
        audioData->input_state_cb = input_cb;
        audioData->user_inputctx = input_ctx;
        audioData->output_state_cb = output_cb;
        audioData->user_outputctx = output_ctx;
        audioData->audio_write_cb = audio_write_cb;
        audioData->user_write_ctx = audio_write_ctx;
        result = AUDIO_RESULT_OK;
    }

    return result;
}

DWORD WINAPI captureThreadProc(
    _In_ LPVOID lpParameter
)
{
    HRESULT hr;
    AUDIO_SYS_DATA* pInput = (AUDIO_SYS_DATA*)lpParameter;
    DWORD waitResult;
    IAudioCaptureClient *pCaptureClient;

    HANDLE allEvents[] = { pInput->spCapture->hCaptureThreadShouldExit, pInput->spCapture->hBufferReady };

    UINT32 packetLength = 0;
    int audio_result = 0;

    AudioDataBuffer audioBuff = { nullptr, 0, 0 };

    if (pInput->input_state_cb)
    {
        pInput->input_state_cb(pInput->user_inputctx, AUDIO_STATE_STARTING);
    }
    pInput->current_input_state = AUDIO_STATE_RUNNING;

    hr = pInput->spCapture->pAudioInputClient->GetService(IID_IAudioCaptureClient,(void**)&pCaptureClient);
    EXIT_ON_ERROR(hr);

    audioBuff.totalSize = pInput->inputFrameCnt * pInput->spCapture->audioInFormat.nBlockAlign;

    audioBuff.pAudioData = new BYTE[audioBuff.totalSize];
    if (!audioBuff.pAudioData)
    {
        goto Exit;
    }

    for (;;)
    {
        waitResult = WaitForMultipleObjects(2, allEvents, false, INFINITE);
        if ((WAIT_OBJECT_0 + 1) == waitResult)
        {
            while (1)
            {
                hr = pCaptureClient->GetNextPacketSize(&packetLength);
                EXIT_ON_ERROR(hr);

                if (packetLength == 0)
                {
                    break;
                }
                hr = GetBufferAndCallBackClient(pCaptureClient,
                    audioBuff,
                    pInput->audio_write_cb,
                    pInput->user_write_ctx,
                    audio_result);
                EXIT_ON_ERROR(hr);

                if (audio_result)
                {
                    pInput->current_input_state = AUDIO_STATE_STOPPED;
                    if (pInput->input_state_cb)
                    {
                        pInput->input_state_cb(pInput->user_inputctx, AUDIO_STATE_STOPPED);
                    }
                }
            }
        }
        else
        {
            goto Exit;
        }
    }

Exit:

    delete [] audioBuff.pAudioData;

    pInput->current_input_state = AUDIO_STATE_STOPPED;
    if (pInput->input_state_cb)
    {
        pInput->input_state_cb(pInput->user_inputctx, AUDIO_STATE_STOPPED);
    }

    SAFE_RELEASE(pCaptureClient);
    return 0;
}


AUDIO_RESULT audio_input_start(AUDIO_SYS_HANDLE handle)
{
    HRESULT hr = E_FAIL;
    AUDIO_SYS_DATA * audioData = (AUDIO_SYS_DATA*)handle;

    if (!audioData || audioData->spCapture->hBufferReady == nullptr)
    {
        return AUDIO_RESULT_INVALID_ARG;
    }

    if (!audioData->audio_write_cb || !audioData->spCapture->pAudioInputClient || audioData->current_input_state == AUDIO_STATE_RUNNING)
    {
        return AUDIO_RESULT_INVALID_STATE;
    }
    if (!audioData->hCaptureThread.joinable())
    {
        audioData->hCaptureThread = std::thread(&captureThreadProc, (LPVOID)audioData);
        EXIT_ON_ERROR_IF(E_FAIL, audioData->hCaptureThread.native_handle() == nullptr);
    }

    // Start recording, Starting the stream causes the IAudioClient object to begin streaming data between the endpoint buffer and the audio engine.
    hr = audioData->spCapture->pAudioInputClient->Start();
    EXIT_ON_ERROR_IF(E_FAIL, hr != S_OK);

Exit:
    return SUCCEEDED(hr) ? AUDIO_RESULT_OK : AUDIO_RESULT_ERROR;
}

AUDIO_RESULT audio_input_stop(AUDIO_SYS_HANDLE handle)
{
    AUDIO_RESULT result;
    if (handle != NULL)
    {
        AUDIO_SYS_DATA* audioData = (AUDIO_SYS_DATA*)handle;
        if (audioData->current_input_state != AUDIO_STATE_RUNNING)
        {
            result = AUDIO_RESULT_INVALID_STATE;
        }
        else
        {
            audioData->spCapture->pAudioInputClient->Stop();

            // Exit our proc thread.
            SetEvent(audioData->spCapture->hCaptureThreadShouldExit);
            if (audioData->hCaptureThread.joinable())
            {
                audioData->hCaptureThread.join();
            }

            result = AUDIO_RESULT_OK;
        }
    }
    else
    {
        result = AUDIO_RESULT_INVALID_ARG;
    }
    return result;
}


AUDIO_RESULT audio_set_options(AUDIO_SYS_HANDLE handle, const char* optionName, const void* value)
{
    if (handle != NULL)
    {
        AUDIO_SYS_DATA* audioData = (AUDIO_SYS_DATA*)handle;

        if (strcmp(AUDIO_OPTION_INPUT_FRAME_COUNT, optionName) == 0)
        {
            if (value)
                audioData->inputFrameCnt = (size_t)*((int*)value);
            else
                audioData->inputFrameCnt = 0;
            return AUDIO_RESULT_OK;
        }
        if (strcmp(AUDIO_OPTION_DEVICENAME, optionName) == 0)
        {
            if (!audioData->hDeviceName)
            {
                if(value)
                    audioData->hDeviceName = STRING_construct(reinterpret_cast<const char*>(value));
                else
                    audioData->hDeviceName = STRING_construct("");
            }
            else
            {
                if (value)
                    STRING_copy(audioData->hDeviceName, (char*)value);
                else
                    STRING_copy(audioData->hDeviceName, "");
            }

            if (audioData->hDeviceName)
            {
                return AUDIO_RESULT_OK;
            }
        }
    }
    return AUDIO_RESULT_INVALID_ARG;

}

AUDIO_RESULT  audio_output_set_volume(AUDIO_SYS_HANDLE handle, long volume)
{
    UNUSED(handle);
    UNUSED(volume);

    return AUDIO_RESULT_OK;
}

AUDIO_RESULT audio_playwavfile(AUDIO_SYS_HANDLE hAudioOut, const char* file)
{
    UNUSED(hAudioOut);
    UNUSED(file);
    return AUDIO_RESULT_OK;
}
