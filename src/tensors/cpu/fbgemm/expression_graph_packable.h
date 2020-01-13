#pragma once

#include "graph/expression_graph.h"
#include "packed_gemm.h"
#include "3rd_party/intgemm/intgemm.h"

namespace marian {

namespace { //Convenient function to get rows and columns of a tensor, shadowed by namespace.
  inline int cols(Tensor& tensor) { return tensor->shape()[-1]; }
  inline int rows(Tensor& tensor) { return tensor->shape().elements() / cols(tensor); }
}

// When FBGEMM based packed GEMM is used, some weight matrices need to be packed offline.
// The decision which weights can be packed or not should be done walking through the graph.
// This requires some more changes, but we temporarily do this just by name ("_W") of the weights.
// And, this introduces a low level packed_gemm.h apis interact with high level graph class.
// So, we make a subclass of ExpressionGraph and put those immature codes in this class.
// We will improve this in the near future. 
class ExpressionGraphPackable : public ExpressionGraph {
public:
  ExpressionGraphPackable()
    : ExpressionGraph( /* inference =  */ true) {} // Packable expression graph only supports inference

  virtual ~ExpressionGraphPackable() {}

  // Convert model weights into packed format and save to IO items.
  // @TODO: review this
  void packAndSave(const std::string& name, const std::string& meta, Type gemmElementType = Type::float32, Type saveElementType = Type::float32) {
    std::vector<io::Item> ioItems;

    // sorted by name in std::map
    for (auto p : params()->getMap()) {
      std::string pName = p.first;

      if (!namespace_.empty()) {
        if (pName.substr(0, namespace_.size() + 2) == namespace_ + "::")
          pName = pName.substr(namespace_.size() + 2);
      }

      Tensor val = p.second->val();

      // save as packed format
      // @TODO Hardcoded to find packable weights - all the weights used for affine op (fp16), all the weights used for affine op and dot op (int8)
      if ((gemmElementType == Type::packed8avx2 || gemmElementType == Type::packed8avx512)
        && (pName.find("_W") == pName.length() - 3 || pName.find("_W") == pName.length() - 2)) {
  #if USE_FBGEMM
        using namespace marian::cpu::variant;
        // packing information - size
        int nrow;
        int ncol;
        uint64_t packsize;

        fbgemmPacked8PackInfo(val->shape(),
                              gemmElementType,
                              pName.find("Wemb") != std::string::npos,
                              nrow,
                              ncol,
                              packsize);

        auto allocator = New<TensorAllocator>(getBackend());

        // buffer tensor to save packed matrix
        Tensor packedTensor;
        allocator->allocate(packedTensor, { 1, (int32_t)packsize }, Type::uint8);

        //Pack B matrix into int8
        fbgemmPacked8Pack(packedTensor,
                          val->data(),
                          gemmElementType,
                          pName.find("Wemb") != std::string::npos,
                          nrow,
                          ncol,
                          packsize);
        io::Item item;
        item.name = pName;
        item.shape = val->shape();
        item.type = gemmElementType;

        // Use the actual memory as this will be aligned and padded.
        // When memory mapping this is required. Shape keeps track of
        // tensor size. Saving to *.npz will cut to size.
        auto mem = packedTensor->memory();
        item.bytes.resize(mem->size());
        copy(backend_, mem->data<char>(), mem->data<char>() + mem->size(), item.bytes.data());

        ioItems.emplace_back(std::move(item));
#else
        ABORT("Packed type {} only supported when compiled with -DUSE_FBGEMM=on", gemmElementType);
#endif
      } else if (gemmElementType == Type::packed16 && pName.find("_W") == pName.length() - 3) {
#if USE_FBGEMM
        using namespace marian::cpu::variant;

        // packing information
        int nrow, ncol, kernel_ncol_blocks, brow, bcol, last_brow, nbrow, nbcol;
        uint64_t packsize;

        fbgemmPacked16PackInfo(val->shape(),
          false,
          nrow,
          ncol,
          kernel_ncol_blocks,
          brow,
          bcol,
          last_brow,
          nbrow,
          nbcol,
          packsize);

        auto allocator = New<TensorAllocator>(getBackend());

        Tensor packedTensor;
        allocator->allocate(packedTensor, { 1, (int32_t)packsize }, Type::uint8);

        // fbgemmPacked16Pack
        fbgemmPacked16Pack(packedTensor,
          val->data(),
          false,
          nrow,
          ncol,
          kernel_ncol_blocks,
          brow,
          bcol,
          last_brow,
          nbrow,
          nbcol,
          packsize);
        io::Item item;
        item.name = pName;
        item.shape = val->shape();
        item.type = gemmElementType;

        // Use the actual memory as this will be aligned and padded.
        // When memory mapping this is required. Shape keeps track of
        // tensor size. Saving to *.npz will cut to size.
        auto mem = packedTensor->memory();
        item.bytes.resize(mem->size());
        copy(backend_, mem->data<char>(), mem->data<char>() + mem->size(), item.bytes.data());

        ioItems.emplace_back(std::move(item));
#else
        ABORT("Packed type {} only supported when compiled with -DUSE_FBGEMM=on", gemmElementType);
#endif
      } else if (gemmElementType == Type::intgemm8 &&
      (pName.find("_W") == pName.length() - 3 || pName.find("_W") == pName.length() - 2 /* || pName.find("Wemb") != std::string::npos*/)) {

        auto allocator = New<TensorAllocator>(getBackend());

        // Allocate a tensor
        Tensor quantMult;
        allocator->allocate(quantMult, { 1, 1 }, Type::float32);

        // Compute QuantMultiplier
        *quantMult->data() = 127.0f / intgemm::MaxAbsolute(val->data(), val->data() + val->shape().elements());

        //Save... Same as the fbgemm case
        io::Item itemQ;
        itemQ.name = pName + "_QuantMult";
        itemQ.shape = val->shape();
        itemQ.type = gemmElementType;

        auto memQ = quantMult->memory();
        itemQ.bytes.resize(memQ->size());
        copy(backend_, memQ->data<char>(), memQ->data<char>() + memQ->size(), itemQ.bytes.data());

        ioItems.emplace_back(std::move(itemQ));

        Tensor paramMat;
        allocator->allocate(paramMat, val->shape(), gemmElementType);

        // Compress matrix. @TODO make this independent of architecture
        intgemm::Int8::PrepareB(val->data(), /*input*/
                                paramMat->data<int8_t>(), /*output*/
                                *quantMult->data(), /*Quant Mult*/
                                rows(val),
                                cols(val));

        //Put the quantMult at the back of the tensor
        //std::cerr << "QuantMult is: " << (127.0f / intgemm::MaxAbsolute(val->data(), val->data() + val->shape().elements())) << std::endl;
        //*(reinterpret_cast<float *>(paramMat->data<int8_t>() + val->shape().elements()) + 1) = 127.0f / intgemm::MaxAbsolute(val->data(), val->data() + val->shape().elements());

        //Save... Same as the fbgemm case
        io::Item item;
        item.name = pName;
        item.shape = val->shape();
        item.type = gemmElementType;

        auto mem = paramMat->memory();
        item.bytes.resize(mem->size());
        copy(backend_, mem->data<char>(), mem->data<char>() + mem->size(), item.bytes.data());

        ioItems.emplace_back(std::move(item));

      } else if (gemmElementType == Type::intgemm16 &&
      (pName.find("_W") == pName.length() - 3 || pName.find("_W") == pName.length() - 2 || pName.find("Wemb") != std::string::npos)) {
        ABORT("Packed type {} not supported yet", gemmElementType);
      } else {
        io::Item item;
        val->get(item, pName);
        item.convert(saveElementType);
        ioItems.emplace_back(std::move(item));
      }
    }

    if (!meta.empty())
      io::addMetaToItems(meta, "special:model.yml", ioItems);
    io::saveItems(name, ioItems);
  }
};

}  // namespace marian