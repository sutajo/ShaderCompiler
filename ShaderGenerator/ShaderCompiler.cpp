#include "pch.h"
#include "ShaderCompiler.h"

using namespace std;
using namespace winrt;

namespace ShaderGenerator
{
  struct ShaderCompilationContext
  {
    const ShaderInfo* Shader;
    const ShaderCompilationArguments* Options;
    queue<const OptionPermutation*> Input;
    bool IsFailed = false;

    mutex InputMutex, OutputMutex, MessagesMutex;
    vector<CompiledShader> Output;
    unordered_set<string> Messages;

    ShaderCompilationContext(const ShaderInfo& info, const ShaderCompilationArguments& options, const vector<OptionPermutation>& permutations) :
      Shader(&info),
      Options(&options)
    {
      for (auto& permutation : permutations)
      {
        Input.push(&permutation);
      }
    }
  };

  struct ShaderDebugName
  {
    uint16_t Flags;
    uint16_t NameLength;
  };

  DWORD WINAPI CompileWorker(LPVOID contextPtr)
  {
    ShaderCompilationContext& context = *((ShaderCompilationContext*)contextPtr);

    while (true)
    {
      //Get work item
      const OptionPermutation* permutation;
      {
        lock_guard<mutex> lock(context.InputMutex);
        if (context.Input.empty()) return 0;

        permutation = context.Input.front();
        context.Input.pop();
      }

      //Define result
      CompiledShader result{};
      result.Key = permutation->Key;

      //Define macros
      vector<D3D_SHADER_MACRO> macros;
      for (auto& define : permutation->Defines)
      {
        macros.push_back({ define.first.c_str(), define.second.c_str() });
      }
      macros.push_back({ nullptr, nullptr });

      //Define compilation flags
      auto flags = 0u;
      if (context.Options->IsDebug)
      {
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_DEBUG_NAME_FOR_BINARY;
      }

      switch (context.Options->OptimizationLevel)
      {
      case -1:
        flags |= D3DCOMPILE_SKIP_OPTIMIZATION;
        break;
      case 0:
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL0;
        break;
      case 1:
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL1;
        break;
      case 2:
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL2;
        break;
      case 3:
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
        break;
      }

      //Run compilation
      com_ptr<ID3DBlob> binary, errors;
      auto success = SUCCEEDED(D3DCompileFromFile(
        context.Shader->Path.c_str(),
        macros.data(),
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        context.Shader->EntryPoint.c_str(),
        context.Shader->Target.c_str(),
        flags,
        0u,
        binary.put(),
        errors.put()));

      //Post process results
      if (success)
      {
        //Get debug information
        if(context.Options->IsDebug && context.Options->UseExternalDebugSymbols)
        {
          com_ptr<ID3DBlob> pdb;
          D3DGetBlobPart(binary->GetBufferPointer(), binary->GetBufferSize(), D3D_BLOB_PDB, 0, pdb.put());

          com_ptr<ID3DBlob> pdbName;
          D3DGetBlobPart(binary->GetBufferPointer(), binary->GetBufferSize(), D3D_BLOB_DEBUG_NAME, 0, pdbName.put());

          if (pdb && pdbName)
          {
            auto pDebugNameData = reinterpret_cast<const ShaderDebugName*>(pdbName->GetBufferPointer());
            auto fileName = reinterpret_cast<const char*>(pDebugNameData + 1);

            result.PdbName = fileName;
            result.PdbData.resize(pdb->GetBufferSize());
            memcpy(result.PdbData.data(), pdb->GetBufferPointer(), pdb->GetBufferSize());
          }

          com_ptr<ID3DBlob> stripped;
          D3DStripShader(binary->GetBufferPointer(), binary->GetBufferSize(), D3DCOMPILER_STRIP_DEBUG_INFO, stripped.put());
          swap(binary, stripped);
        }

        //Save binary
        {
          result.Data.resize(binary->GetBufferSize());
          memcpy(result.Data.data(), binary->GetBufferPointer(), binary->GetBufferSize());
        }
      }
      
      //Print out messages
      stringstream messages{ string((char*)errors->GetBufferPointer(), size_t(errors->GetBufferSize())) };
      string message;
      static regex warningIgnoreRegex(".*: warning X3568: '(target|namespace|entry|option)' : unknown pragma ignored");
      {
        while (getline(messages, message, '\n'))
        {
          if (!regex_match(message, warningIgnoreRegex) && context.Messages.emplace(message).second)
          {
            lock_guard<mutex> lock(context.MessagesMutex);
            printf("%s\n", message.c_str());
          }
        }
      }

      //If successful return binary
      if (success)
      {
        lock_guard<mutex> lock(context.OutputMutex);        
        context.Output.push_back(move(result));
      }
      else
      {
        context.IsFailed = true;
      }
    }

    return 0;
  }

  vector<CompiledShader> ShaderGenerator::CompileShader(const ShaderInfo& shader, const ShaderCompilationArguments& options)
  {
    auto permutations = ShaderOption::Permutate(shader.Options);
    ShaderCompilationContext context{shader, options, permutations};

    printf("Compiling %s at optimization level %d", shader.Path.string().c_str(), options.OptimizationLevel);
    if (options.IsDebug) printf(" with debug symbols");
    printf("...\n Generating %zu shader variants.\n", permutations.size());

    auto threadCount = min(thread::hardware_concurrency(), permutations.size());
    vector<HANDLE> threads(threadCount);
    for (auto& thread : threads)
    {
      thread = CreateThread(nullptr, 0, &CompileWorker, &context, 0, nullptr);
    }

    WaitForMultipleObjects(DWORD(threads.size()), threads.data(), true, INFINITE);
    if (context.IsFailed)
    {
      printf("Shader group compilation failed.\n");
      return {};
    }
    else
    {
      printf("Shader group compilation succeeded.\n");
      return move(context.Output);
    }
  }
}
