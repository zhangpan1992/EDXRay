#pragma once

#include "EDXPrerequisites.h"
#include "../Core/Light.h"
#include "../Core/Sampler.h"
#include "../Core/Sampling.h"
#include "Graphics/Color.h"
#include "Graphics/Texture.h"

#include "SkyLight/ArHosekSkyModel.h"

namespace EDX
{
	namespace RayTracer
	{
		class EnvironmentLight : public Light
		{
		private:
			RefPtr<Texture2D<Color>>			mpMap;
			RefPtr<Sampling::Distribution2D>	mpDistribution;
			Array2f								mLuminance;
			float								mScale;
			float								mRotation;
			const Scene*						mpScene;
			bool								mIsTexture;

		public:
			EnvironmentLight(const Color& intens,
				const Scene* scene,
				const uint sampCount = 1)
				: Light(sampCount)
			{
				mpScene = scene;
				mIsTexture = false;
				mScale = 1.0f;
				mpMap = new ConstantTexture2D<Color>(intens);
			}

			EnvironmentLight(const char* path,
				const Scene* scene,
				const float scale = 1.0f,
				const float rotate = 0.0f,
				const uint sampCount = 1)
				: Light(sampCount)
			{
				mpScene = scene;
				mIsTexture = true;
				mScale = scale;
				mRotation = Math::ToRadians(rotate);
				mpMap = new ImageTexture<Color, Color>(path, 1.0f);

				CalcLuminanceDistribution();
			}

			EnvironmentLight(const Color& turbidity,
				const Color& groundAlbedo,
				const float sunElevation,
				const Scene* scene,
				const float rotate = 0.0f,
				const int resX = 1200,
				const int resY = 600,
				const uint sampCount = 1)
				: Light(sampCount)
			{
				mpScene = scene;
				mIsTexture = true;
				mScale = 1.0f;

				float sunElevationRad = Math::ToRadians(sunElevation);
				mRotation = Math::ToRadians(rotate);

				static const int NUM_CHANNELS = 3;
				ArHosekSkyModelState* skyModelState[NUM_CHANNELS];
				for (auto i = 0; i < NUM_CHANNELS; i++)
					skyModelState[i] = arhosek_rgb_skymodelstate_alloc_init(turbidity[i], groundAlbedo[i], sunElevationRad);

				const float sunZenith = float(Math::EDX_PI_2) - sunElevationRad;
				Array<2, Color> skyRadiance;
				skyRadiance.Init(Vector2i(resX, resY));
				for (auto y = 0; y < resY * 0.5f; y++)
				{
					float v = (y + 0.5f) / float(resY);
					for (auto x = 0; x < resX; x++)
					{
						float u = (x + 0.5f) / float(resX);
						float phi = u * float(Math::EDX_TWO_PI);
						float theta = v * float(Math::EDX_PI);

						float cosGamma = Math::Cos(theta) * Math::Cos(sunZenith)
							+ Math::Sin(theta) * Math::Sin(sunZenith)
							* Math::Cos(phi - float(Math::EDX_PI));
						float gamma = acosf(cosGamma);

						for (auto i = 0; i < NUM_CHANNELS; i++)
						{
							float r = arhosek_tristim_skymodel_radiance(skyModelState[i], theta, gamma, i);
							assert(Math::NumericValid(r));
							skyRadiance[Vector2i(x, y)][i] = r * 0.011f;
							if (gamma < 0.02)
								skyRadiance[Vector2i(x, y)][i] = 3000.0f;
						}
					}
				}

				for (auto i = 0; i < NUM_CHANNELS; i++)
					arhosekskymodelstate_free(skyModelState[i]);

				mpMap = new ImageTexture<Color, Color>(skyRadiance.Data(), resX, resY);

				CalcLuminanceDistribution();
			}

			Color Illuminate(const Vector3& pos,
				const RayTracer::Sample& lightSample,
				Vector3* pDir,
				VisibilityTester* pVisTest,
				float* pPdf,
				float* pCosAtLight = nullptr,
				float* pEmitPdfW = nullptr) const override
			{
				if (mIsTexture)
				{
					float u, v;
					mpDistribution->SampleContinuous(lightSample.u, lightSample.v, &u, &v, pPdf);
					if (*pPdf == 0.0f)
						return Color::BLACK;

					float phi = u * float(Math::EDX_TWO_PI);
					float theta = v * float(Math::EDX_PI);
					float sinTheta = Math::Sin(theta);
					*pPdf = sinTheta != 0.0f ? *pPdf / (2.0f * float(Math::EDX_PI) * float(Math::EDX_PI) * sinTheta) : 0.0f;

					*pDir = Math::SphericalDirection(sinTheta,
						Math::Cos(theta),
						phi);
				}
				else
				{
					Vector3 dir = Sampling::UniformSampleSphere(lightSample.u, lightSample.v);
					*pDir = Vector3(dir.x, dir.z, -dir.y);
					*pPdf = Sampling::UniformSpherePDF();
				}

				if (pCosAtLight)
					*pCosAtLight = 1.f;

				if (pEmitPdfW)
				{
					Vector3 center;
					float radius;
					mpScene->WorldBounds().BoundingSphere(&center, &radius);
					*pEmitPdfW = *pPdf * Sampling::ConcentricDiscPdf() / (radius * radius);
				}

				pVisTest->SetRay(pos, *pDir);

				return Emit(-*pDir);
			}

			Color Sample(const RayTracer::Sample& lightSample1,
				const RayTracer::Sample& lightSample2,
				Ray* pRay,
				Vector3* pNormal,
				float* pPdf,
				float* pDirectPdf = nullptr) const override
			{
				float u, v, mapPdf, sinTheta;

				if (mIsTexture)
				{
					mpDistribution->SampleContinuous(lightSample1.u, lightSample1.v, &u, &v, &mapPdf);
					if (mapPdf == 0.0f)
						return Color::BLACK;

					float phi = u * float(Math::EDX_TWO_PI);
					float theta = v * float(Math::EDX_PI);
					sinTheta = Math::Sin(theta);
					*pNormal = -Math::SphericalDirection(sinTheta,
						Math::Cos(theta),
						phi);
				}
				else
				{
					u = lightSample1.u; v = lightSample1.v;
					*pNormal = -Sampling::UniformSampleSphere(lightSample1.u, lightSample1.v);
					mapPdf = Sampling::UniformSpherePDF();

					float theta = v * float(Math::EDX_PI);
					sinTheta = Math::Sin(theta);
				}

				Vector3 center;
				float radius;
				mpScene->WorldBounds().BoundingSphere(&center, &radius);

				Vector3 v1, v2;
				Math::CoordinateSystem(-*pNormal, &v1, &v2);
				float f1, f2;
				Sampling::ConcentricSampleDisk(lightSample2.u, lightSample2.v, &f1, &f2);

				Vector3 origin = center + radius * (f1 * v1 + f2 * v2);
				*pRay = Ray(origin + radius * -*pNormal, *pNormal);

				float pdfW = sinTheta != 0.0f ? mapPdf / (float(Math::EDX_TWO_PI) * float(Math::EDX_PI) * sinTheta) : 0.0f;
				float pdfA = Sampling::ConcentricDiscPdf() / (radius * radius);
				*pPdf = pdfW * pdfA;
				if (pDirectPdf)
					*pDirectPdf = pdfW;

				Vector2 diff[2] = { Vector2::ZERO, Vector2::ZERO };
				return mpMap->Sample(Vector2(u, v), diff, TextureFilter::TriLinear) * mScale;
			}

			Color Emit(const Vector3& dir,
				const Vector3& normal = Vector3::ZERO,
				float* pPdf = nullptr,
				float* pDirectPdf = nullptr) const override
			{
				Vector3 negDir = -dir;
				float s = Math::SphericalPhi(negDir);
				s += mRotation;
				if (s > float(Math::EDX_TWO_PI))
					s -= float(Math::EDX_TWO_PI);
				s *= float(Math::EDX_INV_2PI);
				s = 1.0f - s;
				float theta = Math::SphericalTheta(negDir);
				float t = theta * float(Math::EDX_INV_PI);

				float directPdf = 0.0f;
				if (pDirectPdf || pPdf)
				{
					float mapPdf = mpDistribution->Pdf(s, t);
					float sinTheta = Math::Sin(theta);
					float pdfW = sinTheta != 0.0f ? mapPdf / (float(Math::EDX_TWO_PI) * float(Math::EDX_PI) * sinTheta) : 0.0f;
					if (pDirectPdf)
						*pDirectPdf = pdfW;

					if (pPdf)
					{
						Vector3 center;
						float radius;
						mpScene->WorldBounds().BoundingSphere(&center, &radius);
						float pdfA = Sampling::ConcentricDiscPdf() / (radius * radius);
						*pPdf = pdfW * pdfA;
					}
				}

				Vector2 diff[2] = { Vector2::ZERO, Vector2::ZERO };
				return mpMap->Sample(Vector2(s, t), diff, TextureFilter::TriLinear) * mScale;
			}

			float Pdf(const Vector3& pos, const Vector3& dir) const override
			{
				if (mIsTexture)
				{
					float theta = Math::SphericalTheta(dir);
					float phi = Math::SphericalPhi(dir);
					float sinTheta = Math::Sin(theta);
					return mpDistribution->Pdf(phi * float(Math::EDX_INV_2PI), theta * float(Math::EDX_INV_PI)) /
						(2.0f * float(Math::EDX_PI) * float(Math::EDX_PI) * sinTheta);
				}
				else
					return Sampling::UniformSpherePDF();
			}

			bool IsEnvironmentLight() const override
			{
				return true;
			}
			bool IsDelta() const override
			{
				return false;
			}
			bool IsFinite() const override
			{
				return false;
			}
			Texture2D<Color>* GetTexture() const
			{
				return mpMap.Ptr();
			}
			bool IsTexture() const
			{
				return mIsTexture;
			}
			float GetRotation() const
			{
				return mRotation;
			}

		private:
			void CalcLuminanceDistribution()
			{
				auto width = mpMap->Width();
				auto height = mpMap->Height();
				mLuminance.Init(Vector2i(width, height));
				for (auto y = 0; y < height; y++)
				{
					float v = (y + 0.5f) / float(height);
					float sinTheta = Math::Sin(float(Math::EDX_PI) * (y + 0.5f) / float(height));
					for (auto x = 0; x < width; x++)
					{
						float u = (x + 0.5f) / float(width);
						u += mRotation * float(Math::EDX_INV_2PI);
						if (u >= 1.0f)
							u -= 1.0f;
						Vector2 diff[2] = { Vector2::ZERO, Vector2::ZERO };
						mLuminance[Vector2i(x, y)] = mpMap->Sample(Vector2(u, v), diff, TextureFilter::Linear).Luminance() * sinTheta;
					}
				}

				mpDistribution = new Sampling::Distribution2D(mLuminance.Data(), width, height);
			}
		};

	}
}