// Developer tool: compile one or more HLSL files with the same path the engine
// takes and print the resulting SPIR-V reflection.  Used to validate the
// D3D11 register -> Vulkan binding mapping against real project shaders.
//
// usage: phx_shadertool [--entry p] [--stage frag|vert|geom|comp] file.hlsl...
#include "rhi/shader_compiler.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::string ReadFile(const char *path, bool *ok)
{
  FILE *f = fopen(path, "rb");
  if (!f)
  {
    *ok = false;
    return {};
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string s;
  s.resize(n);
  if (n > 0 && fread(&s[0], 1, n, f) != (size_t)n)
  {
    fclose(f);
    *ok = false;
    return {};
  }
  fclose(f);
  *ok = true;
  return s;
}

int main(int argc, char **argv)
{
  const char *entry = "p";
  const char *spvOut = nullptr;
  phxvk::ShaderStage stage = phxvk::ShaderStage::Pixel;
  std::vector<const char *> files;
  for (int i = 1; i < argc; i++)
  {
    if (!strcmp(argv[i], "--entry") && i + 1 < argc) entry = argv[++i];
    else if (!strcmp(argv[i], "--stage") && i + 1 < argc)
    {
      i++;
      if (!strcmp(argv[i], "frag")) stage = phxvk::ShaderStage::Pixel;
      else if (!strcmp(argv[i], "vert")) stage = phxvk::ShaderStage::Vertex;
      else if (!strcmp(argv[i], "geom")) stage = phxvk::ShaderStage::Geometry;
      else if (!strcmp(argv[i], "comp")) stage = phxvk::ShaderStage::Compute;
    }
    else if (!strcmp(argv[i], "--spv-out") && i + 1 < argc) spvOut = argv[++i];
    else files.push_back(argv[i]);
  }
  if (files.empty())
  {
    fprintf(stderr, "usage: %s [--entry p] [--stage frag] file.hlsl...\n", argv[0]);
    return 2;
  }

  int failed = 0;
  for (auto path : files)
  {
    bool ok = false;
    std::string src = ReadFile(path, &ok);
    if (!ok)
    {
      printf("FAIL %-40s (cannot read)\n", path);
      failed++;
      continue;
    }
    auto res = phxvk::CompileHLSL(src.c_str(), src.size(), entry, stage, path);
    if (!res.ok)
    {
      printf("FAIL %-40s\n%s\n", path, res.error.c_str());
      failed++;
      continue;
    }
    printf("OK   %-40s %zu words\n", path, res.spirv.size());
    if (spvOut)
    {
      FILE *f = fopen(spvOut, "wb");
      if (f)
      {
        fwrite(res.spirv.data(), 4, res.spirv.size(), f);
        fclose(f);
      }
    }
    for (auto &b : res.reflection.bindings)
      printf("       binding=%-4u %-22s reg=%s%u size=%u\n", b.binding, b.name.c_str(),
             b.regClass == phxvk::RegisterClass::ConstantBuffer ? "b" : b.regClass == phxvk::RegisterClass::Texture ? "t"
                                                         : b.regClass == phxvk::RegisterClass::Sampler   ? "s"
                                                                                                        : "u",
             b.regIndex, b.blockSize);
    for (auto &v : res.reflection.inputs)
      printf("       input loc=%u %-20s fmt=%d\n", v.location, v.name.c_str(), (int)v.format);
  }
  return failed ? 1 : 0;
}
