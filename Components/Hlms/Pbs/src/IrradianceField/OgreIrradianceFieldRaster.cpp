/*
-----------------------------------------------------------------------------
This source file is part of OGRE
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-present Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/

#include "IrradianceField/OgreIrradianceFieldRaster.h"

#include "IrradianceField/OgreIrradianceField.h"

#include "Compositor/OgreCompositorManager2.h"
#include "Compositor/OgreCompositorWorkspace.h"
#include "Compositor/Pass/PassQuad/OgreCompositorPassQuad.h"
#include "Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h"
#include "OgreRoot.h"
#include "OgreDepthBuffer.h"

#include "OgreMaterialManager.h"
#include "OgreTechnique.h"

#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreHlmsManager.h"
#include "OgreLogManager.h"
#include "OgreStringConverter.h"
#include "OgreTextureGpuManager.h"
#include "Vao/OgreConstBufferPacked.h"
#include "Vao/OgreTexBufferPacked.h"
#include "Vao/OgreVaoManager.h"

#define TODO_properConvertWorkspace
#define TODO_setparams

namespace Ogre
{
    IrradianceFieldRaster::IrradianceFieldRaster( IrradianceField *creator ) :
        mCreator( creator ),
        mCubemap( 0 ),
        mDepthCubemap( 0 ),
        mRenderWorkspace( 0 ),
        mConvertToIfdWorkspace( 0 ),
        mPixelFormat( PFG_RGBA8_UNORM_SRGB ),
        mCameraNear( 0.5f ),
        mCameraFar( 500.0f ),
        mFieldOrigin( Vector3::ZERO ),
        mFieldSize( Vector3::UNIT_SCALE ),
        mCamera( 0 ),
        mDepthBufferToCubemapPass( 0 )
    {
        MaterialPtr depthBufferToCubemap = MaterialManager::getSingleton().getByName(
            "IFD/DepthBufferToCubemap", ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME );
        if( depthBufferToCubemap )
        {
            depthBufferToCubemap->load();
            mDepthBufferToCubemapPass = depthBufferToCubemap->getTechnique( 0 )->getPass( 0 );
        }
    }
    //-------------------------------------------------------------------------
    IrradianceFieldRaster::~IrradianceFieldRaster() {}
    //-------------------------------------------------------------------------
    void IrradianceFieldRaster::createWorkspace( void )
    {
        SceneManager *sceneManager = mCreator->mSceneManager;

        TextureGpuManager *textureManager =
            sceneManager->getDestinationRenderSystem()->getTextureGpuManager();

        mCubemap = textureManager->createTexture(
            "IrradianceFieldRaster/Temp/" + StringConverter::toString( mCreator->getId() ),
            GpuPageOutStrategy::Discard, TextureFlags::RenderToTexture, TextureTypes::TypeCube );
        mCubemap->setResolution( 32u, 32u );
        mCubemap->setPixelFormat( mPixelFormat );

        mDepthCubemap = textureManager->createTexture(
            "IrradianceFieldRaster/Depth/" + StringConverter::toString( mCreator->getId() ),
            GpuPageOutStrategy::Discard, TextureFlags::RenderToTexture, TextureTypes::TypeCube );
        mDepthCubemap->copyParametersFrom( mCubemap );
        mDepthCubemap->setPixelFormat( PFG_R32_FLOAT );
        mDepthCubemap->_setDepthBufferDefaults( DepthBuffer::POOL_NO_DEPTH, false, PFG_UNKNOWN );

        mCubemap->scheduleTransitionTo( GpuResidency::Resident );
        mDepthCubemap->scheduleTransitionTo( GpuResidency::Resident );

        mCamera = sceneManager->createCamera(
            "IrradianceFieldRaster/" + StringConverter::toString( mCreator->getId() ), true, true );
        mCamera->setFOVy( Degree( 90 ) );
        mCamera->setAspectRatio( 1 );
        mCamera->setFixedYawAxis( false );
        mCamera->setNearClipDistance( mCameraNear );
        mCamera->setFarClipDistance( mCameraFar );

        CompositorManager2 *compositorManager = mCreator->mRoot->getCompositorManager2();

        CompositorChannelVec channels;
        channels.reserve( 2u );
        channels.push_back( mCubemap );
        channels.push_back( mDepthCubemap );
        mRenderWorkspace =
            compositorManager->addWorkspace( sceneManager, channels, mCamera, mWorkspaceName, false );
        mRenderWorkspace->addListener( this );

        TODO_properConvertWorkspace;
        mConvertToIfdWorkspace =
            compositorManager->addWorkspace( sceneManager, channels, mCamera, mWorkspaceName, false );
    }
    //-------------------------------------------------------------------------
    void IrradianceFieldRaster::destroyWorkspace( void )
    {
        if( !mRenderWorkspace )
            return;

        CompositorManager2 *compositorManager = mCreator->mRoot->getCompositorManager2();

        compositorManager->removeWorkspace( mConvertToIfdWorkspace );
        mConvertToIfdWorkspace = 0;
        compositorManager->removeWorkspace( mRenderWorkspace );
        mRenderWorkspace = 0;

        SceneManager *sceneManager = mCreator->mSceneManager;
        TextureGpuManager *textureManager =
            sceneManager->getDestinationRenderSystem()->getTextureGpuManager();

        textureManager->destroyTexture( mDepthCubemap );
        mDepthCubemap = 0;
        textureManager->destroyTexture( mCubemap );
        mCubemap = 0;

        sceneManager->destroyCamera( mCamera );
        mCamera = 0;
    }
    //-------------------------------------------------------------------------
    Vector3 IrradianceFieldRaster::getProbeCenter( size_t probeIdx ) const
    {
        const IrradianceFieldSettings &settings = mCreator->mSettings;

        Vector3 pos;
        pos.x = probeIdx % settings.mNumProbes[0];
        pos.y =
            ( probeIdx % ( settings.mNumProbes[0] * settings.mNumProbes[1] ) ) / settings.mNumProbes[0];
        pos.z = probeIdx / ( settings.mNumProbes[0] * settings.mNumProbes[1] );
        pos += 0.5f;

        pos /= settings.getNumProbes3f();
        pos += mFieldOrigin;
        pos *= mFieldSize;

        return pos;
    }
    //-------------------------------------------------------------------------
    void IrradianceFieldRaster::renderProbes( uint32 probesPerFrame )
    {
        SceneManager *sceneManager = mCreator->mSceneManager;
        RenderSystem *renderSystem = sceneManager->getDestinationRenderSystem();

        const uint32 oldVisibilityMask = sceneManager->getVisibilityMask();
        sceneManager->setVisibilityMask( 0xffffffff );

        const size_t totalNumProbes = mCreator->mSettings.getTotalNumProbes();
        const size_t numProbesToProcess =
            std::min<size_t>( totalNumProbes - mCreator->mNumProbesProcessed, probesPerFrame );
        const size_t maxProbeToProcess = mCreator->mNumProbesProcessed + numProbesToProcess;

        for( size_t i = mCreator->mNumProbesProcessed; i < maxProbeToProcess; ++i )
        {
            Vector3 probeCenter = getProbeCenter( i );

            mCamera->setPosition( probeCenter );

            if( numProbesToProcess > 2u )
                renderSystem->_beginFrameOnce();

            mRenderWorkspace->_update();

            TODO_setparams;

            mConvertToIfdWorkspace->_update();

            if( numProbesToProcess > 2u )
            {
                renderSystem->_update();
                renderSystem->_endFrameOnce();
            }
        }

        mCreator->mNumProbesProcessed += numProbesToProcess;

        sceneManager->setVisibilityMask( oldVisibilityMask );
    }
    //-------------------------------------------------------------------------
    void IrradianceFieldRaster::passPreExecute( CompositorPass *pass )
    {
        const CompositorPassDef *passDef = pass->getDefinition();
        if( passDef->getType() != PASS_QUAD )
            return;

        OGRE_ASSERT_HIGH( dynamic_cast<CompositorPassQuad *>( pass ) );
        CompositorPassQuad *passQuad = static_cast<CompositorPassQuad *>( pass );
        if( passQuad->getPass() == mDepthBufferToCubemapPass )
        {
            GpuProgramParametersSharedPtr psParams =
                mDepthBufferToCubemapPass->getFragmentProgramParameters();

            const uint32 sliceIdx = std::min<uint32>( passDef->getRtIndex(), 5 );
            psParams->setNamedConstant( "cubemapFaceIdx", sliceIdx );
        }
    }
}  // namespace Ogre
