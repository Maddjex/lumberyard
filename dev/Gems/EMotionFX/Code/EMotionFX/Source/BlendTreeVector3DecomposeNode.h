/*
* All or portions of this file Copyright (c) Amazon.com, Inc. or its affiliates or
* its licensors.
*
* For complete copyright and license terms please see the LICENSE at the root of this
* distribution (the "License"). All use of this software is governed by the License,
* or, if provided, by the license below or the license accompanying this file. Do not
* remove or modify any license notices. This file is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*
*/

#pragma once

// include the required headers
#include "EMotionFXConfig.h"
#include "AnimGraphNode.h"


namespace EMotionFX
{
    /**
     *
     */
    class EMFX_API BlendTreeVector3DecomposeNode
        : public AnimGraphNode
    {
    public:
        AZ_RTTI(BlendTreeVector3DecomposeNode, "{C893AECF-E2D7-47AB-BA47-148B7A2BBE39}", AnimGraphNode)
        AZ_CLASS_ALLOCATOR_DECL

        //
        enum
        {
            INPUTPORT_VECTOR    = 0,
            OUTPUTPORT_X        = 0,
            OUTPUTPORT_Y        = 1,
            OUTPUTPORT_Z        = 2
        };

        enum
        {
            PORTID_INPUT_VECTOR = 0,
            PORTID_OUTPUT_X     = 0,
            PORTID_OUTPUT_Y     = 1,
            PORTID_OUTPUT_Z     = 2,
        };

        BlendTreeVector3DecomposeNode();
        ~BlendTreeVector3DecomposeNode();

        bool InitAfterLoading(AnimGraph* animGraph) override;

        uint32 GetVisualColor() const override          { return MCore::RGBA(128, 255, 128); }

        const char* GetPaletteName() const override;
        AnimGraphObject::ECategory GetPaletteCategory() const override;

        static void Reflect(AZ::ReflectContext* context);

    private:
        void Update(AnimGraphInstance* animGraphInstance, float timePassedInSeconds) override;
    };
}   // namespace EMotionFX

