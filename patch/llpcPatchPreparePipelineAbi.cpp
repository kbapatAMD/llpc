/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2018-2019 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
***********************************************************************************************************************
* @file  llpcPatchPrepareAbi.cpp
* @brief LLPC source file: contains declaration and implementation of class Llpc::PatchPreparePipelineAbi.
***********************************************************************************************************************
*/
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

#include "llpcAbiMetadata.h"
#include "llpcCodeGenManager.h"
#include "llpcGfx6ConfigBuilder.h"
#include "llpcGfx9ConfigBuilder.h"
#include "llpcPatch.h"
#include "llpcPipelineShaders.h"
#include "llpcShaderMerger.h"

#define DEBUG_TYPE "llpc-patch-prepare-pipeline-abi"

using namespace llvm;
using namespace Llpc;

namespace Llpc
{

// =====================================================================================================================
// Pass to prepare the pipeline ABI
class PatchPreparePipelineAbi final : public Patch
{
public:
    static char ID;
    PatchPreparePipelineAbi(
        bool    onlySetCallingConvs = false,
        uint32_t skipStageMask = 0)
        :
        Patch(ID),
        m_onlySetCallingConvs(onlySetCallingConvs),
        m_skipStageMask(skipStageMask)
    {
        initializePipelineShadersPass(*llvm::PassRegistry::getPassRegistry());
        initializePatchPreparePipelineAbiPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module& module) override;

    void getAnalysisUsage(AnalysisUsage& analysisUsage) const override
    {
        analysisUsage.addRequired<PipelineShaders>();
    }

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(PatchPreparePipelineAbi);

    void SetCallingConvs(Module& module);

    void MergeShaderAndSetCallingConvs(Module& module);

    void SetCallingConv(ShaderStage     stage,
                        CallingConv::ID callingConv);

    void SetAbiEntryNames(Module& module);

    void AddAbiMetadata(Module& module);

    Result BuildGraphicsPipelineRegConfig(uint8_t** ppConfig, size_t* pConfigSize);

    Result BuildComputePipelineRegConfig(uint8_t** ppConfig, size_t* pConfigSize);

    // -----------------------------------------------------------------------------------------------------------------

    PipelineShaders*  m_pPipelineShaders;    // API shaders in the pipeline

    bool              m_hasVs;               // Whether the pipeline has vertex shader
    bool              m_hasTcs;              // Whether the pipeline has tessellation control shader
    bool              m_hasTes;              // Whether the pipeline has tessellation evaluation shader
    bool              m_hasGs;               // Whether the pipeline has geometry shader

    GfxIpVersion      m_gfxIp;               // Graphics IP version info

    const bool        m_onlySetCallingConvs; // Whether to only set the calling conventions
    const uint32_t    m_skipStageMask;       // Mask indicating which shader stages should be skipped in processing
};

char PatchPreparePipelineAbi::ID = 0;

} // Llpc

// =====================================================================================================================
// Create pass to prepare the pipeline ABI
ModulePass* Llpc::CreatePatchPreparePipelineAbi(
    bool     onlySetCallingConvs, // Should we only set the calling conventions, or do the full prepare.
    uint32_t skipStageMask)       // Mask of stages to be skipped
{
    return new PatchPreparePipelineAbi(onlySetCallingConvs, skipStageMask);
}

// =====================================================================================================================
// Run the pass on the specified LLVM module.
bool PatchPreparePipelineAbi::runOnModule(
    Module& module) // [in,out] LLVM module to be run on
{
    LLVM_DEBUG(dbgs() << "Run the pass Patch-Prepare-Pipeline-Abi\n");

    Patch::Init(&module);

    m_pPipelineShaders = &getAnalysis<PipelineShaders>();

    m_hasVs  = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageVertex)) != 0);
    m_hasTcs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageTessControl)) != 0);
    m_hasTes = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageTessEval)) != 0);
    m_hasGs = ((m_pContext->GetShaderStageMask() & ShaderStageToMask(ShaderStageGeometry)) != 0);

    m_gfxIp = m_pContext->GetGfxIpVersion();

    // If we've only to set the calling conventions, do that now.
    if (m_onlySetCallingConvs)
    {
        SetCallingConvs(module);
    }
    else
    {
        if (m_gfxIp.major >= 9)
        {
            MergeShaderAndSetCallingConvs(module);
        }

        SetAbiEntryNames(module);

        AddAbiMetadata(module);
    }

    return true; // Modified the module.
}

// =====================================================================================================================
// Set calling convention for the entry-point of each shader (pre-GFX9)
void PatchPreparePipelineAbi::SetCallingConvs(
    Module& module)   // [in] LLVM module
{
    const bool hasTs = (m_hasTcs || m_hasTes);

    // NOTE: For each entry-point, set the calling convention appropriate to the hardware shader stage. The action here
    // depends on the pipeline type.
    SetCallingConv(ShaderStageCompute, CallingConv::AMDGPU_CS);
    SetCallingConv(ShaderStageFragment, CallingConv::AMDGPU_PS);

    if (hasTs && m_hasGs)
    {
        // TS-GS pipeline
        SetCallingConv(ShaderStageVertex, CallingConv::AMDGPU_LS);
        SetCallingConv(ShaderStageTessControl, CallingConv::AMDGPU_HS);
        SetCallingConv(ShaderStageTessEval, CallingConv::AMDGPU_ES);
        SetCallingConv(ShaderStageGeometry, CallingConv::AMDGPU_GS);
        SetCallingConv(ShaderStageCopyShader, CallingConv::AMDGPU_VS);
    }
    else if (hasTs)
    {
        // TS-only pipeline
        SetCallingConv(ShaderStageVertex, CallingConv::AMDGPU_LS);
        SetCallingConv(ShaderStageTessControl, CallingConv::AMDGPU_HS);
        SetCallingConv(ShaderStageTessEval, CallingConv::AMDGPU_VS);
    }
    else if (m_hasGs)
    {
        // GS-only pipeline
        SetCallingConv(ShaderStageVertex, CallingConv::AMDGPU_ES);
        SetCallingConv(ShaderStageGeometry, CallingConv::AMDGPU_GS);
        SetCallingConv(ShaderStageCopyShader, CallingConv::AMDGPU_VS);
    }
    else if (m_hasVs)
    {
        // VS-FS pipeine
        SetCallingConv(ShaderStageVertex, CallingConv::AMDGPU_VS);
    }
}

// =====================================================================================================================
// Merge shaders and set calling convention for the entry-point of each each shader (GFX9+)
void PatchPreparePipelineAbi::MergeShaderAndSetCallingConvs(
    Module& module)   // [in] LLVM module
{
    LLPC_ASSERT(m_gfxIp.major >= 9);

    const bool hasTs = (m_hasTcs || m_hasTes);

    // NOTE: For each entry-point, set the calling convention appropriate to the hardware shader stage. The action here
    // depends on the pipeline type, and, for GFX9+, may involve merging shaders.
    SetCallingConv(ShaderStageCompute, CallingConv::AMDGPU_CS);
    SetCallingConv(ShaderStageFragment, CallingConv::AMDGPU_PS);

    if (m_pContext->IsGraphics())
    {
        ShaderMerger shaderMerger(m_pContext, m_pPipelineShaders);
#if LLPC_BUILD_GFX10
        const bool enableNgg = m_pContext->GetNggControl()->enableNgg;
#endif

        if (hasTs && m_hasGs)
        {
            // TS-GS pipeline
            if (m_hasTcs)
            {
                auto pLsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageVertex);
                auto pHsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageTessControl);

                if (pHsEntryPoint != nullptr)
                {
                    auto pLsHsEntryPoint = shaderMerger.GenerateLsHsEntryPoint(pLsEntryPoint, pHsEntryPoint);
                    pLsHsEntryPoint->setCallingConv(CallingConv::AMDGPU_HS);
                }
            }

            auto pEsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageTessEval);
            auto pGsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageGeometry);

#if LLPC_BUILD_GFX10
            if (enableNgg)
            {
                if (pGsEntryPoint != nullptr)
                {
                    auto pPrimShaderEntryPoint = shaderMerger.BuildPrimShader(pEsEntryPoint, pGsEntryPoint);
                    pPrimShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
                }
            }
            else
#endif
            {
                if (pGsEntryPoint != nullptr)
                {
                    auto pEsGsEntryPoint = shaderMerger.GenerateEsGsEntryPoint(pEsEntryPoint, pGsEntryPoint);
                    pEsGsEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
                }

                SetCallingConv(ShaderStageCopyShader, CallingConv::AMDGPU_VS);
            }
        }
        else if (hasTs)
        {
            // TS-only pipeline
            if (m_hasTcs)
            {
                auto pLsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageVertex);
                auto pHsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageTessControl);

                if (pHsEntryPoint != nullptr)
                {
                    auto pLsHsEntryPoint = shaderMerger.GenerateLsHsEntryPoint(pLsEntryPoint, pHsEntryPoint);
                    pLsHsEntryPoint->setCallingConv(CallingConv::AMDGPU_HS);
                }
            }

#if LLPC_BUILD_GFX10
            if (enableNgg)
            {
                // If NGG is enabled, ES-GS merged shader should be present even if GS is absent
                auto pEsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageTessEval);

                if (pEsEntryPoint != nullptr)
                {
                    auto pPrimShaderEntryPoint = shaderMerger.BuildPrimShader(pEsEntryPoint, nullptr);
                    pPrimShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
                }
            }
            else
#endif
            {
                SetCallingConv(ShaderStageTessEval, CallingConv::AMDGPU_VS);
            }
        }
        else if (m_hasGs)
        {
            // GS-only pipeline
            auto pEsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageVertex);
            auto pGsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageGeometry);

#if LLPC_BUILD_GFX10
            if (enableNgg)
            {
                if (pGsEntryPoint != nullptr)
                {
                    auto pPrimShaderEntryPoint = shaderMerger.BuildPrimShader(pEsEntryPoint, pGsEntryPoint);
                    pPrimShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
                }
            }
            else
#endif
            {
                if (pGsEntryPoint != nullptr)
                {
                    auto pEsGsEntryPoint = shaderMerger.GenerateEsGsEntryPoint(pEsEntryPoint, pGsEntryPoint);
                    pEsGsEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
                }

                SetCallingConv(ShaderStageCopyShader, CallingConv::AMDGPU_VS);
            }
        }
        else if (m_hasVs)
        {
            // VS_FS pipeline
#if LLPC_BUILD_GFX10
            if (enableNgg)
            {
                // If NGG is enabled, ES-GS merged shader should be present even if GS is absent
                auto pEsEntryPoint = m_pPipelineShaders->GetEntryPoint(ShaderStageVertex);
                if (pEsEntryPoint != nullptr)
                {
                    auto pPrimShaderEntryPoint = shaderMerger.BuildPrimShader(pEsEntryPoint, nullptr);
                    pPrimShaderEntryPoint->setCallingConv(CallingConv::AMDGPU_GS);
                }
            }
            else
#endif
            {
                SetCallingConv(ShaderStageVertex, CallingConv::AMDGPU_VS);
            }
        }
    }
}

// =====================================================================================================================
// Set calling convention on a particular API shader stage, if that stage has a shader
void PatchPreparePipelineAbi::SetCallingConv(
    ShaderStage     shaderStage,  // Shader stage
    CallingConv::ID callingConv)  // Calling convention to set it to
{
    auto pEntryPoint = m_pPipelineShaders->GetEntryPoint(shaderStage);
    if (pEntryPoint != nullptr)
    {
        if (m_skipStageMask & ShaderStageToMask(shaderStage))
        {
            pEntryPoint->setLinkage(GlobalValue::InternalLinkage);
            pEntryPoint->setCallingConv(CallingConv::C);
        }
        else
        {
            pEntryPoint->setCallingConv(callingConv);
        }
    }
}

// =====================================================================================================================
// Set ABI-specified entrypoint name for each shader
void PatchPreparePipelineAbi::SetAbiEntryNames(
    Module& module)   // [in] LLVM module
{
    for (auto& func : module)
    {
        if (func.empty() == false)
        {
            auto callingConv = func.getCallingConv();
            auto entryStage = Util::Abi::PipelineSymbolType::CsMainEntry;

            switch (callingConv)
            {
            case CallingConv::AMDGPU_CS:
                entryStage = Util::Abi::PipelineSymbolType::CsMainEntry;
                break;
            case CallingConv::AMDGPU_PS:
                entryStage = Util::Abi::PipelineSymbolType::PsMainEntry;
                break;
            case CallingConv::AMDGPU_VS:
                entryStage = Util::Abi::PipelineSymbolType::VsMainEntry;
                break;
            case CallingConv::AMDGPU_GS:
                entryStage = Util::Abi::PipelineSymbolType::GsMainEntry;
                break;
            case CallingConv::AMDGPU_ES:
                entryStage = Util::Abi::PipelineSymbolType::EsMainEntry;
                break;
            case CallingConv::AMDGPU_HS:
                entryStage = Util::Abi::PipelineSymbolType::HsMainEntry;
                break;
            case CallingConv::AMDGPU_LS:
                entryStage = Util::Abi::PipelineSymbolType::LsMainEntry;
                break;
            default:
                continue;
            }
            const char* pEntryName = Util::Abi::PipelineAbiSymbolNameStrings[static_cast<uint32_t>(entryStage)];
            func.setName(pEntryName);
        }
    }
}

// =====================================================================================================================
// Add ABI metadata
void PatchPreparePipelineAbi::AddAbiMetadata(
    Module& module)   // [in] LLVM module
{
    if (m_gfxIp.major <= 8)
    {
        Gfx6::ConfigBuilder configBuilder(&module);
        configBuilder.BuildPalMetadata();
    }
    else
    {
        Gfx9::ConfigBuilder configBuilder(&module);
        configBuilder.BuildPalMetadata();
    }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(PatchPreparePipelineAbi, DEBUG_TYPE, "Patch LLVM for preparing pipeline ABI", false, false)

