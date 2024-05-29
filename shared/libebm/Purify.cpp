// Copyright (c) 2023 The InterpretML Contributors
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "pch.hpp"

#include <stdlib.h> // free
#include <stddef.h> // size_t, ptrdiff_t
#include <string.h> // memcpy
#include <cmath> // std::abs

#define ZONE_main
#include "zones.h"

#include "RandomDeterministic.hpp" // RandomDeterministic

namespace DEFINED_ZONE_NAME {
#ifndef DEFINED_ZONE_NAME
#error DEFINED_ZONE_NAME must be defined
#endif // DEFINED_ZONE_NAME

extern ErrorEbm PurifyInternal(const size_t cTensorBins,
      const double tolerance,
      const size_t cDimensions,
      const IntEbm* const aDimensionLengths,
      const double* const aWeights,
      double* const aScores,
      double* const aImpurities,
      double* const pInterceptOut) {
   EBM_ASSERT(1 <= cTensorBins);
   EBM_ASSERT(1 <= cDimensions);
   EBM_ASSERT(nullptr != aDimensionLengths);
   EBM_ASSERT(nullptr != aWeights);
   EBM_ASSERT(nullptr != aScores);
   EBM_ASSERT(nullptr != aImpurities);

   const double* pScore = aScores;
   const double* pWeight = aWeights;
   const double* const aScoresEnd = aScores + cTensorBins;
   double impurityMax = 0.0;
   double impurityTotalAll = 0.0;
   double weightTotalAll = 0.0;
   do {
      const double weight = *pWeight;
      const double score = *pScore;
      weightTotalAll += weight;
      const double impurity = score * weight;
      impurityTotalAll += impurity;
      impurityMax += std::abs(impurity);
      ++pScore;
      ++pWeight;
   } while(aScoresEnd != pScore);

   if(0.0 == weightTotalAll) {
      return Error_None;
   }

   impurityMax = impurityMax * tolerance / weightTotalAll;

   if(nullptr != pInterceptOut) {
      // pull out the intercept early since this will make purification easier
      const double intercept = impurityTotalAll / weightTotalAll;
      *pInterceptOut = intercept;
      const double interceptNeg = -intercept;
      double* pScore2 = aScores;
      do {
         *pScore2 += interceptNeg;
         ++pScore2;
      } while(aScoresEnd != pScore2);
   }

   size_t cSurfaceBins = 0;
   size_t iExclude = 0;
   do {
      const size_t cBins = static_cast<size_t>(aDimensionLengths[iExclude]);
      EBM_ASSERT(0 == cTensorBins % cBins);
      const size_t cSurfaceBinsExclude = cTensorBins / cBins;
      cSurfaceBins += cSurfaceBinsExclude;
      ++iExclude;
   } while(cDimensions != iExclude);

   memset(aImpurities, 0, cSurfaceBins * sizeof(*aImpurities));

   double impurityPrev = std::numeric_limits<double>::infinity();
   double impurityCur;
   bool bRetry;
   do {
      impurityCur = 0.0;
      bRetry = false;

      // TODO: do a card shuffle of the surface bin indexes to process them in random order

      for(size_t iAllSurfaceBin = 0; iAllSurfaceBin < cSurfaceBins; ++iAllSurfaceBin) {
         size_t cTensorIncrement = 1;
         size_t iSweepDimension = 0;
         size_t cSweepBins;
         size_t iDimensionSurfaceBin = iAllSurfaceBin;
         while(true) {
            cSweepBins = static_cast<size_t>(aDimensionLengths[iSweepDimension]);
            EBM_ASSERT(0 == cTensorBins % cSweepBins);
            size_t cSurfaceBinsExclude = cTensorBins / cSweepBins;
            if(iDimensionSurfaceBin < cSurfaceBinsExclude) {
               // we've found it
               break;
            }
            iDimensionSurfaceBin -= cSurfaceBinsExclude;
            cTensorIncrement *= cSweepBins;
            ++iSweepDimension;
            EBM_ASSERT(iSweepDimension < cDimensions);
         }

         size_t iTensor = 0;
         size_t multiple = 1;
         for(size_t iDimension = 0; iDimension < cDimensions; ++iDimension) {
            const size_t cBins = static_cast<size_t>(aDimensionLengths[iDimension]);
            if(iDimension != iSweepDimension) {
               const size_t iBin = iDimensionSurfaceBin % cBins;
               iDimensionSurfaceBin /= cBins;
               iTensor += iBin * multiple;
            }
            multiple *= cBins;
         }
         EBM_ASSERT(0 == iDimensionSurfaceBin); // TODO: we could exit early on this condition in the future

         const size_t iTensorEnd = iTensor + cTensorIncrement * cSweepBins;
         double impurity = 0;
         double weightTotal = 0;
         for(size_t iTensorCur = iTensor; iTensorCur != iTensorEnd; iTensorCur += cTensorIncrement) {
            const double weight = aWeights[iTensorCur];
            const double score = aScores[iTensorCur];

            impurity += score * weight;
            weightTotal += weight;
         }

         impurity = 0.0 == weightTotal ? 0.0 : impurity / weightTotal;

         const double absImpurity = std::abs(impurity);
         bRetry |= impurityMax < absImpurity;
         impurityCur += absImpurity;

         aImpurities[iAllSurfaceBin] += impurity;
         impurity = -impurity;

         for(size_t iTensorCur = iTensor; iTensorCur != iTensorEnd; iTensorCur += cTensorIncrement) {
            aScores[iTensorCur] += impurity;
         }
      }

      if(impurityPrev <= impurityCur) {
         // To ensure that we exit even with floating point noise, exit when things do not improve.
         break;
      }
      impurityPrev = impurityCur;
   } while(bRetry);

   return Error_None;
}


EBM_API_BODY ErrorEbm EBM_CALLING_CONVENTION Purify(double tolerance,
      IntEbm countDimensions,
      const IntEbm* dimensionLengths,
      const double* weights,
      double* scores,
      double* impurities,
      double* interceptOut) {
   LOG_N(Trace_Info,
         "Entered Purify: "
         "tolerance=%le, "
         "countDimensions=%" IntEbmPrintf ", "
         "dimensionLengths=%p, "
         "weights=%p, "
         "scores=%p, "
         "impurities=%p, "
         "interceptOut=%p",
         tolerance,
         countDimensions,
         static_cast<const void*>(dimensionLengths),
         static_cast<const void*>(weights),
         static_cast<const void*>(scores),
         static_cast<const void*>(impurities),
         static_cast<const void*>(interceptOut));

   ErrorEbm error;

   if(nullptr != interceptOut) {
      *interceptOut = 0.0;
   }

   if(countDimensions <= IntEbm{0}) {
      if(IntEbm{0} == countDimensions) {
         LOG_0(Trace_Info, "INFO Purify zero dimensions");
         return Error_None;
      } else {
         LOG_0(Trace_Error, "ERROR Purify countDimensions must be positive");
         return Error_IllegalParamVal;
      }
   }
   if(IntEbm{k_cDimensionsMax} < countDimensions) {
      LOG_0(Trace_Warning, "WARNING Purify countDimensions too large and would cause out of memory condition");
      return Error_OutOfMemory;
   }
   const size_t cDimensions = static_cast<size_t>(countDimensions);

   if(nullptr == dimensionLengths) {
      LOG_0(Trace_Error, "ERROR Purify nullptr == dimensionLengths");
      return Error_IllegalParamVal;
   }

   bool bZero = false;
   size_t iDimension = 0;
   do {
      const IntEbm dimensionsLength = dimensionLengths[iDimension];
      if(dimensionsLength <= IntEbm{0}) {
         if(dimensionsLength < IntEbm{0}) {
            LOG_0(Trace_Error, "ERROR Purify dimensionsLength value cannot be negative");
            return Error_IllegalParamVal;
         }
         bZero = true;
      }
      ++iDimension;
   } while(cDimensions != iDimension);
   if(bZero) {
      LOG_0(Trace_Info, "INFO Purify empty tensor");
      return Error_None;
   }

   iDimension = 0;
   size_t cTensorBins = 1;
   do {
      const IntEbm dimensionsLength = dimensionLengths[iDimension];
      EBM_ASSERT(IntEbm{1} <= dimensionsLength);
      if(IsConvertError<size_t>(dimensionsLength)) {
         // the scores tensor could not exist with this many tensor bins, so it is an error
         LOG_0(Trace_Error, "ERROR Purify IsConvertError<size_t>(dimensionsLength)");
         return Error_OutOfMemory;
      }
      const size_t cBins = static_cast<size_t>(dimensionsLength);

      if(IsMultiplyError(cTensorBins, cBins)) {
         // the scores tensor could not exist with this many tensor bins, so it is an error
         LOG_0(Trace_Error, "ERROR Purify IsMultiplyError(cTensorBins, cBins)");
         return Error_OutOfMemory;
      }
      cTensorBins *= cBins;

      ++iDimension;
   } while(cDimensions != iDimension);
   EBM_ASSERT(1 <= cTensorBins);

   if(nullptr == weights) {
      LOG_0(Trace_Error, "ERROR Purify nullptr == weights");
      return Error_IllegalParamVal;
   }

   if(nullptr == scores) {
      LOG_0(Trace_Error, "ERROR Purify nullptr == scores");
      return Error_IllegalParamVal;
   }

   if(nullptr == impurities) {
      LOG_0(Trace_Error, "ERROR Purify nullptr == impurities");
      return Error_IllegalParamVal;
   }

   error = PurifyInternal(
         cTensorBins, tolerance, cDimensions, dimensionLengths, weights, scores, impurities, interceptOut);

   LOG_0(Trace_Info, "Exited Purify");

   return error;
}

} // namespace DEFINED_ZONE_NAME
