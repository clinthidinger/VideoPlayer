//
//  AX-MediaPlayerMSWDXGIRenderPath.cxx
//  AX-MediaPlayer
//
//  Created by Andrew Wright (@axjxwright) on 17/08/21.
//  (c) 2021 AX Interactive (axinteractive.com.au)
//


#pragma comment(lib, "d3d11.lib")

#include "AX-MediaPlayerMSWDXGIRenderPath.h"
#if (CINDER_VERSION < 903)
    // Cinder 0.9.2 and younger used GLLoad
    #include "glload/wgl_all.h"
#else
    // Cinder 0.9.3 and (presumably) going forward uses GLAD
    #include "glad/glad_wgl.h"
#endif

#include "cinder/app/App.h"

using namespace ci;

namespace AX::Video
{
    using SharedTexture                 = DXGIRenderPath::SharedTexture;
    using SharedTextureRef              = DXGIRenderPath::SharedTextureRef;
    using InteropContextRef             = std::unique_ptr<class InteropContext>;

    // @note(andrew): Have a single D3D + interop context for all video sessions
    class InteropContext                : public ci::Noncopyable
    {
    public:

        static void                     StaticInitialize ( );
        static InteropContext &         Get ( );

        ~InteropContext                 ( );

        inline ID3D11Device *           Device ( ) const { return _device.Get(); }
        inline HANDLE                   Handle ( ) const { return _interopHandle; }
        inline IMFDXGIDeviceManager *   DXGIManager ( ) const { return _dxgiManager.Get ( ); }
        
        SharedTextureRef                CreateSharedTexture ( const ivec2 & size );
        inline bool                     IsValid ( ) const { return _isValid; }
        
    protected:

        InteropContext                  ( );

        ComPtr<ID3D11Device>            _device{ nullptr };
        ComPtr<IMFDXGIDeviceManager>    _dxgiManager{ nullptr };
        UINT                            _dxgiResetToken{ 0 };

        HANDLE                          _interopHandle{ nullptr };
        bool                            _isValid{ false };
    };

    // @leak(andrew): Lazily initialize and deliberately leak this 
    // to make sure it hangs around for the remainder of the application
    // It has to outlive any of the players that depend on it being alive and valid
    // @todo(andrew): Find a less gross way to manage this lifetime

    static InteropContext * kInteropContext{ nullptr };
    void InteropContext::StaticInitialize ( )
    {
        if ( !kInteropContext )
        {
            kInteropContext = new InteropContext ( );
        }
    }

    InteropContext & InteropContext::Get ( )
    {
        assert ( kInteropContext );
        return *kInteropContext;
    }

    class DXGIRenderPath::SharedTexture
    {
    public:

        SharedTexture               ( const ivec2 & size );
        ~SharedTexture              ( );

        bool                        Lock ( );
        bool                        Unlock ( );
        inline bool                 IsLocked ( ) const { return _isLocked;  }

        inline bool IsValid         ( ) const { return _isValid; }
        ID3D11Texture2D *           DXTextureHandle ( ) const { return _dxTexture.Get( ); }
        const ci::gl::TextureRef &  GLTextureHandle ( ) const { return _glTexture; }
        LONGLONG                    GetPresentationTimestamp() const { return _presentationTimestamp; }
        void                        SetPresentationTimestamp( LONGLONG pts ) { _presentationTimestamp = pts; }

    protected:

        ci::gl::TextureRef          _glTexture;
        ComPtr<ID3D11Texture2D>     _dxTexture{ nullptr };
        HANDLE                      _shareHandle{ nullptr };
        bool                        _isValid{ false };
        bool                        _isLocked{ false };
        LONGLONG                    _presentationTimestamp{ -1 };
    };

    class DXGIRenderPathFrameLease : public MediaPlayer::FrameLease
    {
    public:

        DXGIRenderPathFrameLease ( const SharedTextureRef & texture )
            : _texture ( texture.get ( ) )
        {
            if ( _texture ) _texture->Lock ( );
        }

        inline bool    IsValid   ( ) const override { return ToTexture ( ) != nullptr; }
        gl::TextureRef ToTexture ( ) const override { return _texture ? _texture->GLTextureHandle ( ) : nullptr; };
        LONGLONG GetPresentationTimeStamp() const override { return _texture ? _texture->GetPresentationTimestamp() : -1; }

        ~DXGIRenderPathFrameLease ( )
        {
            if ( _texture && _texture->IsLocked ( ) )
            {
                _texture->Unlock ( );
                _texture = nullptr;
            }
        }

    protected:

        SharedTexture * _texture{ nullptr };
    };

    InteropContext::InteropContext ( )
        : _isValid ( false )
    {
        UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

#ifndef NDEBUG
        deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        if ( !SUCCEEDED ( MFCreateDXGIDeviceManager ( &_dxgiResetToken, _dxgiManager.GetAddressOf ( ) ) ) ) return;
        if ( !SUCCEEDED ( D3D11CreateDevice ( nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, nullptr, 0, D3D11_SDK_VERSION, _device.GetAddressOf ( ), nullptr, nullptr ) ) ) return;
        
        ComPtr<ID3D10Multithread> multiThread{ nullptr };
        if ( SUCCEEDED ( _device->QueryInterface ( multiThread.GetAddressOf ( ) ) ) )
        {
            multiThread->SetMultithreadProtected ( true );
        }
        else
        {
            return;
        }

        if ( !SUCCEEDED ( _dxgiManager->ResetDevice ( _device.Get ( ), _dxgiResetToken ) ) ) return;
        
        ID3D11Device *d = _device.Get();
        auto hglrc = wglGetCurrentContext();
        auto dc = wglGetCurrentDC();
        bool isNull = ( wglDXOpenDeviceNV == nullptr );
        _interopHandle = wglDXOpenDeviceNV ( _device.Get ( ) );
        _isValid = _interopHandle != nullptr;
    }

    SharedTextureRef InteropContext::CreateSharedTexture ( const ivec2 & size )
    {
        auto texture = std::make_unique<SharedTexture> ( size );
        if ( texture->IsValid ( ) ) return std::move ( texture );

        return nullptr;
    }

    InteropContext::~InteropContext ( )
    {
        if ( _interopHandle != nullptr )
        {
            wglDXCloseDeviceNV ( _interopHandle );
            _interopHandle = nullptr;
        }

        _dxgiManager = nullptr;
        
        // @leak(andrew): Debug layer is whinging about live objects but is this 
        // this because the ComPtr destructors haven't had a chance to fire yet?
        #ifndef NDEBUG
        if ( _device )
        {
            ComPtr<ID3D11Debug> debug{ nullptr };
            if ( SUCCEEDED ( _device->QueryInterface ( debug.GetAddressOf ( ) ) ) )
            {
                debug->ReportLiveDeviceObjects ( D3D11_RLDO_DETAIL | D3D11_RLDO_IGNORE_INTERNAL );
            }
        }
        #endif
    }

    DXGIRenderPath::SharedTexture::SharedTexture ( const ivec2 & size )
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = size.x;
        desc.Height = size.y;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        desc.Usage = D3D11_USAGE_DEFAULT;

        auto & context = InteropContext::Get ( );

        if ( SUCCEEDED ( context.Device()->CreateTexture2D ( &desc, nullptr, _dxTexture.GetAddressOf ( ) ) ) )
        {
            gl::Texture::Format fmt;
            fmt.internalFormat ( GL_RGBA ).loadTopDown ( );
            
            _glTexture = gl::Texture::create ( size.x, size.y, fmt );
            _shareHandle = wglDXRegisterObjectNV ( context.Handle(), _dxTexture.Get(), _glTexture->getId(), GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV );
            _isValid = _shareHandle != nullptr;
        }
    }

    bool DXGIRenderPath::SharedTexture::Lock ( )
    {
        assert ( !IsLocked ( ) );
        _isLocked = wglDXLockObjectsNV ( InteropContext::Get().Handle ( ), 1, &_shareHandle );
        return _isLocked;
    }

    bool DXGIRenderPath::SharedTexture::Unlock ( )
    {
        assert ( IsLocked ( ) );
        if ( wglDXUnlockObjectsNV ( InteropContext::Get ( ).Handle ( ), 1, &_shareHandle ) )
        {
            _isLocked = false;
            return true;
        }

        return false;
    }

    DXGIRenderPath::SharedTexture::~SharedTexture ( )
    {
        if ( _shareHandle != nullptr )
        { 
            if ( wglGetCurrentContext() != nullptr ) // No GL Context, so we're likely in the process of shutting down
            {
                if ( IsLocked() ) wglDXUnlockObjectsNV ( InteropContext::Get().Handle(), 1, &_shareHandle );
                wglDXUnregisterObjectNV ( InteropContext::Get().Handle(), _shareHandle );
                _shareHandle = nullptr;
            }
        }
    }

    DXGIRenderPath::DXGIRenderPath ( MediaPlayer::Impl & owner, const ci::DataSourceRef & source )
        : RenderPath ( owner, source )
    { }

    bool DXGIRenderPath::Initialize ( IMFAttributes & attributes )
    {
        InteropContext::StaticInitialize ( );

        auto & interop = InteropContext::Get ( );
        if ( !interop.IsValid ( ) ) return false;

        if ( SUCCEEDED ( attributes.SetUnknown ( MF_MEDIA_ENGINE_DXGI_MANAGER, interop.DXGIManager() ) ) )
        {
            return true;
        }

        return false;
    }

    bool DXGIRenderPath::InitializeRenderTarget ( const ci::ivec2 & size )
    {
        if ( !_sharedTexture || size != _size )
        {
            _size = size;
            _sharedTexture = InteropContext::Get ( ).CreateSharedTexture ( size );
        }

        return ( _sharedTexture != nullptr );
    }

    bool DXGIRenderPath::ProcessFrame ( LONGLONG presentationTimestamp )
    {
        if ( _sharedTexture )
        {
            auto & engine = _owner._mediaEngine;

            MFVideoNormalizedRect srcRect{ 0.0f, 0.0f, 1.0f, 1.0f };
            RECT dstRect{ 0, 0, _size.x, _size.y };
            MFARGB black{ 0, 0, 0, 0 };

            bool ok = SUCCEEDED ( engine->TransferVideoFrame ( _sharedTexture->DXTextureHandle(), &srcRect, &dstRect, &black ) );
            if ( ok )
            {
                _owner._hasNewFrame.store ( true );
            }
            _sharedTexture->SetPresentationTimestamp( presentationTimestamp );

            return ok;
        }

        return false;
    }

    MediaPlayer::FrameLeaseRef DXGIRenderPath::GetFrameLease ( ) const
    {
        return std::make_unique<DXGIRenderPathFrameLease> ( _sharedTexture );
    }

    DXGIRenderPath::~DXGIRenderPath ( )
    {
        _sharedTexture = nullptr;
    }
}