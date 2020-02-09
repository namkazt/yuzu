// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/shader_type.h"
#include "video_core/renderer_opengl/gl_device.h"
#include "video_core/renderer_opengl/gl_rasterizer.h"
#include "video_core/renderer_opengl/gl_shader_decompiler.h"
#include "video_core/shader/ast.h"
#include "video_core/shader/node.h"
#include "video_core/shader/shader_ir.h"

namespace OpenGL::GLShader {

namespace {

using Tegra::Engines::ShaderType;
using Tegra::Shader::Attribute;
using Tegra::Shader::AttributeUse;
using Tegra::Shader::Header;
using Tegra::Shader::IpaInterpMode;
using Tegra::Shader::IpaMode;
using Tegra::Shader::IpaSampleMode;
using Tegra::Shader::Register;

using namespace std::string_literals;
using namespace VideoCommon::Shader;

using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using Operation = const OperationNode&;

class ASTDecompiler;
class ExprDecompiler;

enum class Type { Void, Bool, Bool2, Float, Int, Uint, HalfFloat };

struct TextureOffset {};
struct TextureDerivates {};
using TextureArgument = std::pair<Type, Node>;
using TextureIR = std::variant<TextureOffset, TextureDerivates, TextureArgument>;

constexpr u32 MAX_CONSTBUFFER_ELEMENTS =
    static_cast<u32>(Maxwell::MaxConstBufferSize) / (4 * sizeof(float));

class ShaderWriter final {
public:
    void AddExpression(std::string_view text) {
        DEBUG_ASSERT(scope >= 0);
        if (!text.empty()) {
            AppendIndentation();
        }
        shader_source += text;
    }

    // Forwards all arguments directly to libfmt.
    // Note that all formatting requirements for fmt must be
    // obeyed when using this function. (e.g. {{ must be used
    // printing the character '{' is desirable. Ditto for }} and '}',
    // etc).
    template <typename... Args>
    void AddLine(std::string_view text, Args&&... args) {
        AddExpression(fmt::format(text, std::forward<Args>(args)...));
        AddNewLine();
    }

    void AddNewLine() {
        DEBUG_ASSERT(scope >= 0);
        shader_source += '\n';
    }

    std::string GenerateTemporary() {
        return fmt::format("tmp{}", temporary_index++);
    }

    std::string GetResult() {
        return std::move(shader_source);
    }

    s32 scope = 0;

private:
    void AppendIndentation() {
        shader_source.append(static_cast<std::size_t>(scope) * 4, ' ');
    }

    std::string shader_source;
    u32 temporary_index = 1;
};

class Expression final {
public:
    Expression(std::string code, Type type) : code{std::move(code)}, type{type} {
        ASSERT(type != Type::Void);
    }
    Expression() : type{Type::Void} {}

    Type GetType() const {
        return type;
    }

    std::string GetCode() const {
        return code;
    }

    void CheckVoid() const {
        ASSERT(type == Type::Void);
    }

    std::string As(Type type) const {
        switch (type) {
        case Type::Bool:
            return AsBool();
        case Type::Bool2:
            return AsBool2();
        case Type::Float:
            return AsFloat();
        case Type::Int:
            return AsInt();
        case Type::Uint:
            return AsUint();
        case Type::HalfFloat:
            return AsHalfFloat();
        default:
            UNREACHABLE_MSG("Invalid type");
            return code;
        }
    }

    std::string AsBool() const {
        switch (type) {
        case Type::Bool:
            return code;
        default:
            UNREACHABLE_MSG("Incompatible types");
            return code;
        }
    }

    std::string AsBool2() const {
        switch (type) {
        case Type::Bool2:
            return code;
        default:
            UNREACHABLE_MSG("Incompatible types");
            return code;
        }
    }

    std::string AsFloat() const {
        switch (type) {
        case Type::Float:
            return code;
        case Type::Uint:
            return fmt::format("utof({})", code);
        case Type::Int:
            return fmt::format("itof({})", code);
        case Type::HalfFloat:
            return fmt::format("utof(packHalf2x16({}))", code);
        default:
            UNREACHABLE_MSG("Incompatible types");
            return code;
        }
    }

    std::string AsInt() const {
        switch (type) {
        case Type::Float:
            return fmt::format("ftoi({})", code);
        case Type::Uint:
            return fmt::format("int({})", code);
        case Type::Int:
            return code;
        case Type::HalfFloat:
            return fmt::format("int(packHalf2x16({}))", code);
        default:
            UNREACHABLE_MSG("Incompatible types");
            return code;
        }
    }

    std::string AsUint() const {
        switch (type) {
        case Type::Float:
            return fmt::format("ftou({})", code);
        case Type::Uint:
            return code;
        case Type::Int:
            return fmt::format("uint({})", code);
        case Type::HalfFloat:
            return fmt::format("packHalf2x16({})", code);
        default:
            UNREACHABLE_MSG("Incompatible types");
            return code;
        }
    }

    std::string AsHalfFloat() const {
        switch (type) {
        case Type::Float:
            return fmt::format("unpackHalf2x16(ftou({}))", code);
        case Type::Uint:
            return fmt::format("unpackHalf2x16({})", code);
        case Type::Int:
            return fmt::format("unpackHalf2x16(int({}))", code);
        case Type::HalfFloat:
            return code;
        default:
            UNREACHABLE_MSG("Incompatible types");
            return code;
        }
    }

private:
    std::string code;
    Type type{};
};

const char* GetTypeString(Type type) {
    switch (type) {
    case Type::Bool:
        return "bool";
    case Type::Bool2:
        return "bvec2";
    case Type::Float:
        return "float";
    case Type::Int:
        return "int";
    case Type::Uint:
        return "uint";
    case Type::HalfFloat:
        return "vec2";
    default:
        UNREACHABLE_MSG("Invalid type");
        return "<invalid type>";
    }
}

const char* GetImageTypeDeclaration(Tegra::Shader::ImageType image_type) {
    switch (image_type) {
    case Tegra::Shader::ImageType::Texture1D:
        return "1D";
    case Tegra::Shader::ImageType::TextureBuffer:
        return "Buffer";
    case Tegra::Shader::ImageType::Texture1DArray:
        return "1DArray";
    case Tegra::Shader::ImageType::Texture2D:
        return "2D";
    case Tegra::Shader::ImageType::Texture2DArray:
        return "2DArray";
    case Tegra::Shader::ImageType::Texture3D:
        return "3D";
    default:
        UNREACHABLE();
        return "1D";
    }
}

/// Generates code to use for a swizzle operation.
constexpr const char* GetSwizzle(u32 element) {
    constexpr std::array swizzle = {".x", ".y", ".z", ".w"};
    return swizzle.at(element);
}

/// Translate topology
std::string GetTopologyName(Tegra::Shader::OutputTopology topology) {
    switch (topology) {
    case Tegra::Shader::OutputTopology::PointList:
        return "points";
    case Tegra::Shader::OutputTopology::LineStrip:
        return "line_strip";
    case Tegra::Shader::OutputTopology::TriangleStrip:
        return "triangle_strip";
    default:
        UNIMPLEMENTED_MSG("Unknown output topology: {}", static_cast<u32>(topology));
        return "points";
    }
}

/// Returns true if an object has to be treated as precise
bool IsPrecise(Operation operand) {
    const auto& meta{operand.GetMeta()};
    if (const auto arithmetic = std::get_if<MetaArithmetic>(&meta)) {
        return arithmetic->precise;
    }
    return false;
}

bool IsPrecise(const Node& node) {
    if (const auto operation = std::get_if<OperationNode>(&*node)) {
        return IsPrecise(*operation);
    }
    return false;
}

constexpr bool IsGenericAttribute(Attribute::Index index) {
    return index >= Attribute::Index::Attribute_0 && index <= Attribute::Index::Attribute_31;
}

constexpr Attribute::Index ToGenericAttribute(u64 value) {
    return static_cast<Attribute::Index>(value + static_cast<u64>(Attribute::Index::Attribute_0));
}

u32 GetGenericAttributeIndex(Attribute::Index index) {
    ASSERT(IsGenericAttribute(index));
    return static_cast<u32>(index) - static_cast<u32>(Attribute::Index::Attribute_0);
}

constexpr const char* GetFlowStackPrefix(MetaStackClass stack) {
    switch (stack) {
    case MetaStackClass::Ssy:
        return "ssy";
    case MetaStackClass::Pbk:
        return "pbk";
    }
    return {};
}

std::string FlowStackName(MetaStackClass stack) {
    return fmt::format("{}_flow_stack", GetFlowStackPrefix(stack));
}

std::string FlowStackTopName(MetaStackClass stack) {
    return fmt::format("{}_flow_stack_top", GetFlowStackPrefix(stack));
}

[[deprecated]] constexpr bool IsVertexShader(ShaderType stage) {
    return stage == ShaderType::Vertex;
}

class GLSLDecompiler final {
public:
    explicit GLSLDecompiler(const Device& device, const ShaderIR& ir, ShaderType stage,
                            std::string suffix)
        : device{device}, ir{ir}, stage{stage}, suffix{suffix}, header{ir.GetHeader()} {}

    void DecompileBranchMode() {
        // VM's program counter
        const auto first_address = ir.GetBasicBlocks().begin()->first;
        code.AddLine("uint jmp_to = {}U;", first_address);

        // TODO(Subv): Figure out the actual depth of the flow stack, for now it seems
        // unlikely that shaders will use 20 nested SSYs and PBKs.
        constexpr u32 FLOW_STACK_SIZE = 20;
        if (!ir.IsFlowStackDisabled()) {
            for (const auto stack : std::array{MetaStackClass::Ssy, MetaStackClass::Pbk}) {
                code.AddLine("uint {}[{}];", FlowStackName(stack), FLOW_STACK_SIZE);
                code.AddLine("uint {} = 0U;", FlowStackTopName(stack));
            }
        }

        code.AddLine("while (true) {{");
        ++code.scope;

        code.AddLine("switch (jmp_to) {{");

        for (const auto& pair : ir.GetBasicBlocks()) {
            const auto [address, bb] = pair;
            code.AddLine("case 0x{:X}U: {{", address);
            ++code.scope;

            VisitBlock(bb);

            --code.scope;
            code.AddLine("}}");
        }

        code.AddLine("default: return;");
        code.AddLine("}}");

        --code.scope;
        code.AddLine("}}");
    }

    void DecompileAST();

    void Decompile() {
        DeclareVertex();
        DeclareGeometry();
        DeclareRegisters();
        DeclareCustomVariables();
        DeclarePredicates();
        DeclareLocalMemory();
        DeclareInternalFlags();
        DeclareInputAttributes();
        DeclareOutputAttributes();
        DeclareConstantBuffers();
        DeclareGlobalMemory();
        DeclareSamplers();
        DeclareImages();
        DeclarePhysicalAttributeReader();

        code.AddLine("void execute_{}() {{", suffix);
        ++code.scope;

        if (ir.IsDecompiled()) {
            DecompileAST();
        } else {
            DecompileBranchMode();
        }

        --code.scope;
        code.AddLine("}}");
    }

    std::string GetResult() {
        return code.GetResult();
    }

private:
    friend class ASTDecompiler;
    friend class ExprDecompiler;

    void DeclareVertex() {
        if (!IsVertexShader(stage))
            return;

        DeclareVertexRedeclarations();
    }

    void DeclareGeometry() {
        if (stage != ShaderType::Geometry) {
            return;
        }

        const auto topology = GetTopologyName(header.common3.output_topology);
        const auto max_vertices = header.common4.max_output_vertices.Value();
        code.AddLine("layout ({}, max_vertices = {}) out;", topology, max_vertices);
        code.AddNewLine();

        code.AddLine("in gl_PerVertex {{");
        ++code.scope;
        code.AddLine("vec4 gl_Position;");
        --code.scope;
        code.AddLine("}} gl_in[];");

        DeclareVertexRedeclarations();
    }

    void DeclareVertexRedeclarations() {
        code.AddLine("out gl_PerVertex {{");
        ++code.scope;

        code.AddLine("vec4 gl_Position;");

        for (const auto attribute : ir.GetOutputAttributes()) {
            if (attribute == Attribute::Index::ClipDistances0123 ||
                attribute == Attribute::Index::ClipDistances4567) {
                code.AddLine("float gl_ClipDistance[];");
                break;
            }
        }
        if (!IsVertexShader(stage) || device.HasVertexViewportLayer()) {
            if (ir.UsesLayer()) {
                code.AddLine("int gl_Layer;");
            }
            if (ir.UsesViewportIndex()) {
                code.AddLine("int gl_ViewportIndex;");
            }
        } else if ((ir.UsesLayer() || ir.UsesViewportIndex()) && IsVertexShader(stage) &&
                   !device.HasVertexViewportLayer()) {
            LOG_ERROR(
                Render_OpenGL,
                "GL_ARB_shader_viewport_layer_array is not available and its required by a shader");
        }

        if (ir.UsesPointSize()) {
            code.AddLine("float gl_PointSize;");
        }

        if (ir.UsesInstanceId()) {
            code.AddLine("int gl_InstanceID;");
        }

        if (ir.UsesVertexId()) {
            code.AddLine("int gl_VertexID;");
        }

        --code.scope;
        code.AddLine("}};");
        code.AddNewLine();
    }

    void DeclareRegisters() {
        const auto& registers = ir.GetRegisters();
        for (const u32 gpr : registers) {
            code.AddLine("float {} = 0.0f;", GetRegister(gpr));
        }
        if (!registers.empty()) {
            code.AddNewLine();
        }
    }

    void DeclareCustomVariables() {
        const u32 num_custom_variables = ir.GetNumCustomVariables();
        for (u32 i = 0; i < num_custom_variables; ++i) {
            code.AddLine("float {} = 0.0f;", GetCustomVariable(i));
        }
        if (num_custom_variables > 0) {
            code.AddNewLine();
        }
    }

    void DeclarePredicates() {
        const auto& predicates = ir.GetPredicates();
        for (const auto pred : predicates) {
            code.AddLine("bool {} = false;", GetPredicate(pred));
        }
        if (!predicates.empty()) {
            code.AddNewLine();
        }
    }

    void DeclareLocalMemory() {
        if (stage == ShaderType::Compute) {
            code.AddLine("#ifdef LOCAL_MEMORY_SIZE");
            code.AddLine("uint {}[LOCAL_MEMORY_SIZE];", GetLocalMemory());
            code.AddLine("#endif");
            return;
        }

        const u64 local_memory_size = header.GetLocalMemorySize();
        if (local_memory_size == 0) {
            return;
        }
        const auto element_count = Common::AlignUp(local_memory_size, 4) / 4;
        code.AddLine("uint {}[{}];", GetLocalMemory(), element_count);
        code.AddNewLine();
    }

    void DeclareInternalFlags() {
        for (u32 flag = 0; flag < static_cast<u32>(InternalFlag::Amount); flag++) {
            const auto flag_code = static_cast<InternalFlag>(flag);
            code.AddLine("bool {} = false;", GetInternalFlag(flag_code));
        }
        code.AddNewLine();
    }

    std::string GetInputFlags(AttributeUse attribute) {
        switch (attribute) {
        case AttributeUse::Perspective:
            // Default, Smooth
            return {};
        case AttributeUse::Constant:
            return "flat ";
        case AttributeUse::ScreenLinear:
            return "noperspective ";
        default:
        case AttributeUse::Unused:
            UNIMPLEMENTED_MSG("Unknown attribute usage index={}", static_cast<u32>(attribute));
            return {};
        }
    }

    void DeclareInputAttributes() {
        if (ir.HasPhysicalAttributes()) {
            const u32 num_inputs{GetNumPhysicalInputAttributes()};
            for (u32 i = 0; i < num_inputs; ++i) {
                DeclareInputAttribute(ToGenericAttribute(i), true);
            }
            code.AddNewLine();
            return;
        }

        const auto& attributes = ir.GetInputAttributes();
        for (const auto index : attributes) {
            if (IsGenericAttribute(index)) {
                DeclareInputAttribute(index, false);
            }
        }
        if (!attributes.empty()) {
            code.AddNewLine();
        }
    }

    void DeclareInputAttribute(Attribute::Index index, bool skip_unused) {
        const u32 location{GetGenericAttributeIndex(index)};

        std::string name{GetInputAttribute(index)};
        if (stage == ShaderType::Geometry) {
            name = "gs_" + name + "[]";
        }

        std::string suffix;
        if (stage == ShaderType::Fragment) {
            const auto input_mode{header.ps.GetAttributeUse(location)};
            if (skip_unused && input_mode == AttributeUse::Unused) {
                return;
            }
            suffix = GetInputFlags(input_mode);
        }

        code.AddLine("layout (location = {}) {} in vec4 {};", location, suffix, name);
    }

    void DeclareOutputAttributes() {
        if (ir.HasPhysicalAttributes() && stage != ShaderType::Fragment) {
            for (u32 i = 0; i < GetNumPhysicalVaryings(); ++i) {
                DeclareOutputAttribute(ToGenericAttribute(i));
            }
            code.AddNewLine();
            return;
        }

        const auto& attributes = ir.GetOutputAttributes();
        for (const auto index : attributes) {
            if (IsGenericAttribute(index)) {
                DeclareOutputAttribute(index);
            }
        }
        if (!attributes.empty()) {
            code.AddNewLine();
        }
    }

    void DeclareOutputAttribute(Attribute::Index index) {
        const u32 location{GetGenericAttributeIndex(index)};
        code.AddLine("layout (location = {}) out vec4 {};", location, GetOutputAttribute(index));
    }

    void DeclareConstantBuffers() {
        u32 binding = device.GetBaseBindings(stage).uniform_buffer;
        for (const auto& [index, cbuf] : ir.GetConstantBuffers()) {
            code.AddLine("layout (std140, binding = {}) uniform {} {{", binding++,
                         GetConstBufferBlock(index));
            code.AddLine("    uvec4 {}[{}];", GetConstBuffer(index), MAX_CONSTBUFFER_ELEMENTS);
            code.AddLine("}};");
            code.AddNewLine();
        }
    }

    void DeclareGlobalMemory() {
        u32 binding = device.GetBaseBindings(stage).shader_storage_buffer;
        for (const auto& [base, usage] : ir.GetGlobalMemory()) {
            // Since we don't know how the shader will use the shader, hint the driver to disable as
            // much optimizations as possible
            std::string qualifier = "coherent volatile";
            if (usage.is_read && !usage.is_written) {
                qualifier += " readonly";
            } else if (usage.is_written && !usage.is_read) {
                qualifier += " writeonly";
            }

            code.AddLine("layout (std430, binding = {}) {} buffer {} {{", binding++, qualifier,
                         GetGlobalMemoryBlock(base));
            code.AddLine("    uint {}[];", GetGlobalMemory(base));
            code.AddLine("}};");
            code.AddNewLine();
        }
    }

    void DeclareSamplers() {
        u32 binding = device.GetBaseBindings(stage).sampler;
        for (const auto& sampler : ir.GetSamplers()) {
            const std::string name = GetSampler(sampler);
            const std::string description = fmt::format("layout (binding = {}) uniform", binding);
            binding += sampler.IsIndexed() ? sampler.Size() : 1;

            std::string sampler_type = [&]() {
                if (sampler.IsBuffer()) {
                    return "samplerBuffer";
                }
                switch (sampler.GetType()) {
                case Tegra::Shader::TextureType::Texture1D:
                    return "sampler1D";
                case Tegra::Shader::TextureType::Texture2D:
                    return "sampler2D";
                case Tegra::Shader::TextureType::Texture3D:
                    return "sampler3D";
                case Tegra::Shader::TextureType::TextureCube:
                    return "samplerCube";
                default:
                    UNREACHABLE();
                    return "sampler2D";
                }
            }();
            if (sampler.IsArray()) {
                sampler_type += "Array";
            }
            if (sampler.IsShadow()) {
                sampler_type += "Shadow";
            }

            if (!sampler.IsIndexed()) {
                code.AddLine("{} {} {};", description, sampler_type, name);
            } else {
                code.AddLine("{} {} {}[{}];", description, sampler_type, name, sampler.Size());
            }
        }
        if (!ir.GetSamplers().empty()) {
            code.AddNewLine();
        }
    }

    void DeclarePhysicalAttributeReader() {
        if (!ir.HasPhysicalAttributes()) {
            return;
        }
        code.AddLine("float ReadPhysicalAttribute(uint physical_address) {{");
        ++code.scope;
        code.AddLine("switch (physical_address) {{");

        // Just declare generic attributes for now.
        const auto num_attributes{static_cast<u32>(GetNumPhysicalInputAttributes())};
        for (u32 index = 0; index < num_attributes; ++index) {
            const auto attribute{ToGenericAttribute(index)};
            for (u32 element = 0; element < 4; ++element) {
                constexpr u32 generic_base = 0x80;
                constexpr u32 generic_stride = 16;
                constexpr u32 element_stride = 4;
                const u32 address{generic_base + index * generic_stride + element * element_stride};

                const bool declared = stage != ShaderType::Fragment ||
                                      header.ps.GetAttributeUse(index) != AttributeUse::Unused;
                const std::string value =
                    declared ? ReadAttribute(attribute, element).AsFloat() : "0.0f";
                code.AddLine("case 0x{:X}U: return {};", address, value);
            }
        }

        code.AddLine("default: return 0;");

        code.AddLine("}}");
        --code.scope;
        code.AddLine("}}");
        code.AddNewLine();
    }

    void DeclareImages() {
        u32 binding = device.GetBaseBindings(stage).image;
        for (const auto& image : ir.GetImages()) {
            std::string qualifier = "coherent volatile";
            if (image.IsRead() && !image.IsWritten()) {
                qualifier += " readonly";
            } else if (image.IsWritten() && !image.IsRead()) {
                qualifier += " writeonly";
            }

            const char* format = image.IsAtomic() ? "r32ui, " : "";
            const char* type_declaration = GetImageTypeDeclaration(image.GetType());
            code.AddLine("layout ({}binding = {}) {} uniform uimage{} {};", format, binding++,
                         qualifier, type_declaration, GetImage(image));
        }
        if (!ir.GetImages().empty()) {
            code.AddNewLine();
        }
    }

    void VisitBlock(const NodeBlock& bb) {
        for (const auto& node : bb) {
            Visit(node).CheckVoid();
        }
    }

    Expression Visit(const Node& node) {
        if (const auto operation = std::get_if<OperationNode>(&*node)) {
            if (const auto amend_index = operation->GetAmendIndex()) {
                Visit(ir.GetAmendNode(*amend_index)).CheckVoid();
            }
            const auto operation_index = static_cast<std::size_t>(operation->GetCode());
            if (operation_index >= operation_decompilers.size()) {
                UNREACHABLE_MSG("Out of bounds operation: {}", operation_index);
                return {};
            }
            const auto decompiler = operation_decompilers[operation_index];
            if (decompiler == nullptr) {
                UNREACHABLE_MSG("Undefined operation: {}", operation_index);
                return {};
            }
            return (this->*decompiler)(*operation);
        }

        if (const auto gpr = std::get_if<GprNode>(&*node)) {
            const u32 index = gpr->GetIndex();
            if (index == Register::ZeroIndex) {
                return {"0U", Type::Uint};
            }
            return {GetRegister(index), Type::Float};
        }

        if (const auto cv = std::get_if<CustomVarNode>(&*node)) {
            const u32 index = cv->GetIndex();
            return {GetCustomVariable(index), Type::Float};
        }

        if (const auto immediate = std::get_if<ImmediateNode>(&*node)) {
            const u32 value = immediate->GetValue();
            if (value < 10) {
                // For eyecandy avoid using hex numbers on single digits
                return {fmt::format("{}U", immediate->GetValue()), Type::Uint};
            }
            return {fmt::format("0x{:X}U", immediate->GetValue()), Type::Uint};
        }

        if (const auto predicate = std::get_if<PredicateNode>(&*node)) {
            const auto value = [&]() -> std::string {
                switch (const auto index = predicate->GetIndex(); index) {
                case Tegra::Shader::Pred::UnusedIndex:
                    return "true";
                case Tegra::Shader::Pred::NeverExecute:
                    return "false";
                default:
                    return GetPredicate(index);
                }
            }();
            if (predicate->IsNegated()) {
                return {fmt::format("!({})", value), Type::Bool};
            }
            return {value, Type::Bool};
        }

        if (const auto abuf = std::get_if<AbufNode>(&*node)) {
            UNIMPLEMENTED_IF_MSG(abuf->IsPhysicalBuffer() && stage == ShaderType::Geometry,
                                 "Physical attributes in geometry shaders are not implemented");
            if (abuf->IsPhysicalBuffer()) {
                return {fmt::format("ReadPhysicalAttribute({})",
                                    Visit(abuf->GetPhysicalAddress()).AsUint()),
                        Type::Float};
            }
            return ReadAttribute(abuf->GetIndex(), abuf->GetElement(), abuf->GetBuffer());
        }

        if (const auto cbuf = std::get_if<CbufNode>(&*node)) {
            const Node offset = cbuf->GetOffset();
            if (const auto immediate = std::get_if<ImmediateNode>(&*offset)) {
                // Direct access
                const u32 offset_imm = immediate->GetValue();
                ASSERT_MSG(offset_imm % 4 == 0, "Unaligned cbuf direct access");
                return {fmt::format("{}[{}][{}]", GetConstBuffer(cbuf->GetIndex()),
                                    offset_imm / (4 * 4), (offset_imm / 4) % 4),
                        Type::Uint};
            }

            if (std::holds_alternative<OperationNode>(*offset)) {
                // Indirect access
                const std::string final_offset = code.GenerateTemporary();
                code.AddLine("uint {} = {} >> 2;", final_offset, Visit(offset).AsUint());

                if (!device.HasComponentIndexingBug()) {
                    return {fmt::format("{}[{} >> 2][{} & 3]", GetConstBuffer(cbuf->GetIndex()),
                                        final_offset, final_offset),
                            Type::Uint};
                }

                // AMD's proprietary GLSL compiler emits ill code for variable component access.
                // To bypass this driver bug generate 4 ifs, one per each component.
                const std::string pack = code.GenerateTemporary();
                code.AddLine("uvec4 {} = {}[{} >> 2];", pack, GetConstBuffer(cbuf->GetIndex()),
                             final_offset);

                const std::string result = code.GenerateTemporary();
                code.AddLine("uint {};", result);
                for (u32 swizzle = 0; swizzle < 4; ++swizzle) {
                    code.AddLine("if (({} & 3) == {}) {} = {}{};", final_offset, swizzle, result,
                                 pack, GetSwizzle(swizzle));
                }
                return {result, Type::Uint};
            }

            UNREACHABLE_MSG("Unmanaged offset node type");
        }

        if (const auto gmem = std::get_if<GmemNode>(&*node)) {
            const std::string real = Visit(gmem->GetRealAddress()).AsUint();
            const std::string base = Visit(gmem->GetBaseAddress()).AsUint();
            const std::string final_offset = fmt::format("({} - {}) >> 2", real, base);
            return {fmt::format("{}[{}]", GetGlobalMemory(gmem->GetDescriptor()), final_offset),
                    Type::Uint};
        }

        if (const auto lmem = std::get_if<LmemNode>(&*node)) {
            return {
                fmt::format("{}[{} >> 2]", GetLocalMemory(), Visit(lmem->GetAddress()).AsUint()),
                Type::Uint};
        }

        if (const auto smem = std::get_if<SmemNode>(&*node)) {
            return {fmt::format("smem[{} >> 2]", Visit(smem->GetAddress()).AsUint()), Type::Uint};
        }

        if (const auto internal_flag = std::get_if<InternalFlagNode>(&*node)) {
            return {GetInternalFlag(internal_flag->GetFlag()), Type::Bool};
        }

        if (const auto conditional = std::get_if<ConditionalNode>(&*node)) {
            if (const auto amend_index = conditional->GetAmendIndex()) {
                Visit(ir.GetAmendNode(*amend_index)).CheckVoid();
            }
            // It's invalid to call conditional on nested nodes, use an operation instead
            code.AddLine("if ({}) {{", Visit(conditional->GetCondition()).AsBool());
            ++code.scope;

            VisitBlock(conditional->GetCode());

            --code.scope;
            code.AddLine("}}");
            return {};
        }

        if (const auto comment = std::get_if<CommentNode>(&*node)) {
            code.AddLine("// " + comment->GetText());
            return {};
        }

        UNREACHABLE();
        return {};
    }

    Expression ReadAttribute(Attribute::Index attribute, u32 element, const Node& buffer = {}) {
        const auto GeometryPass = [&](std::string_view name) {
            if (stage == ShaderType::Geometry && buffer) {
                // TODO(Rodrigo): Guard geometry inputs against out of bound reads. Some games
                // set an 0x80000000 index for those and the shader fails to build. Find out why
                // this happens and what's its intent.
                return fmt::format("gs_{}[{} % MAX_VERTEX_INPUT]", name, Visit(buffer).AsUint());
            }
            return std::string(name);
        };

        switch (attribute) {
        case Attribute::Index::Position:
            switch (stage) {
            case ShaderType::Geometry:
                return {fmt::format("gl_in[{}].gl_Position{}", Visit(buffer).AsUint(),
                                    GetSwizzle(element)),
                        Type::Float};
            case ShaderType::Fragment:
                return {element == 3 ? "1.0f" : ("gl_FragCoord"s + GetSwizzle(element)),
                        Type::Float};
            default:
                UNREACHABLE();
            }
        case Attribute::Index::PointCoord:
            switch (element) {
            case 0:
                return {"gl_PointCoord.x", Type::Float};
            case 1:
                return {"gl_PointCoord.y", Type::Float};
            case 2:
            case 3:
                return {"0.0f", Type::Float};
            }
            UNREACHABLE();
            return {"0", Type::Int};
        case Attribute::Index::TessCoordInstanceIDVertexID:
            // TODO(Subv): Find out what the values are for the first two elements when inside a
            // vertex shader, and what's the value of the fourth element when inside a Tess Eval
            // shader.
            ASSERT(IsVertexShader(stage));
            switch (element) {
            case 2:
                // Config pack's first value is instance_id.
                return {"gl_InstanceID", Type::Int};
            case 3:
                return {"gl_VertexID", Type::Int};
            }
            UNIMPLEMENTED_MSG("Unmanaged TessCoordInstanceIDVertexID element={}", element);
            return {"0", Type::Int};
        case Attribute::Index::FrontFacing:
            // TODO(Subv): Find out what the values are for the other elements.
            ASSERT(stage == ShaderType::Fragment);
            switch (element) {
            case 3:
                return {"(gl_FrontFacing ? -1 : 0)", Type::Int};
            }
            UNIMPLEMENTED_MSG("Unmanaged FrontFacing element={}", element);
            return {"0", Type::Int};
        default:
            if (IsGenericAttribute(attribute)) {
                return {GeometryPass(GetInputAttribute(attribute)) + GetSwizzle(element),
                        Type::Float};
            }
            break;
        }
        UNIMPLEMENTED_MSG("Unhandled input attribute: {}", static_cast<u32>(attribute));
        return {"0", Type::Int};
    }

    Expression ApplyPrecise(Operation operation, std::string value, Type type) {
        if (!IsPrecise(operation)) {
            return {std::move(value), type};
        }
        // Old Nvidia drivers have a bug with precise and texture sampling. These are more likely to
        // be found in fragment shaders, so we disable precise there. There are vertex shaders that
        // also fail to build but nobody seems to care about those.
        // Note: Only bugged drivers will skip precise.
        const bool disable_precise = device.HasPreciseBug() && stage == ShaderType::Fragment;

        std::string temporary = code.GenerateTemporary();
        code.AddLine("{}{} {} = {};", disable_precise ? "" : "precise ", GetTypeString(type),
                     temporary, value);
        return {std::move(temporary), type};
    }

    Expression VisitOperand(Operation operation, std::size_t operand_index) {
        const auto& operand = operation[operand_index];
        const bool parent_precise = IsPrecise(operation);
        const bool child_precise = IsPrecise(operand);
        const bool child_trivial = !std::holds_alternative<OperationNode>(*operand);
        if (!parent_precise || child_precise || child_trivial) {
            return Visit(operand);
        }

        Expression value = Visit(operand);
        std::string temporary = code.GenerateTemporary();
        code.AddLine("{} {} = {};", GetTypeString(value.GetType()), temporary, value.GetCode());
        return {std::move(temporary), value.GetType()};
    }

    std::optional<Expression> GetOutputAttribute(const AbufNode* abuf) {
        switch (const auto attribute = abuf->GetIndex()) {
        case Attribute::Index::Position:
            return {{"gl_Position"s + GetSwizzle(abuf->GetElement()), Type::Float}};
        case Attribute::Index::LayerViewportPointSize:
            switch (abuf->GetElement()) {
            case 0:
                UNIMPLEMENTED();
                return {};
            case 1:
                if (IsVertexShader(stage) && !device.HasVertexViewportLayer()) {
                    return {};
                }
                return {{"gl_Layer", Type::Int}};
            case 2:
                if (IsVertexShader(stage) && !device.HasVertexViewportLayer()) {
                    return {};
                }
                return {{"gl_ViewportIndex", Type::Int}};
            case 3:
                return {{"gl_PointSize", Type::Float}};
            }
            return {};
        case Attribute::Index::ClipDistances0123:
            return {{fmt::format("gl_ClipDistance[{}]", abuf->GetElement()), Type::Float}};
        case Attribute::Index::ClipDistances4567:
            return {{fmt::format("gl_ClipDistance[{}]", abuf->GetElement() + 4), Type::Float}};
        default:
            if (IsGenericAttribute(attribute)) {
                return {
                    {GetOutputAttribute(attribute) + GetSwizzle(abuf->GetElement()), Type::Float}};
            }
            UNIMPLEMENTED_MSG("Unhandled output attribute: {}", static_cast<u32>(attribute));
            return {};
        }
    }

    Expression GenerateUnary(Operation operation, std::string_view func, Type result_type,
                             Type type_a) {
        std::string op_str = fmt::format("{}({})", func, VisitOperand(operation, 0).As(type_a));
        return ApplyPrecise(operation, std::move(op_str), result_type);
    }

    Expression GenerateBinaryInfix(Operation operation, std::string_view func, Type result_type,
                                   Type type_a, Type type_b) {
        const std::string op_a = VisitOperand(operation, 0).As(type_a);
        const std::string op_b = VisitOperand(operation, 1).As(type_b);
        std::string op_str = fmt::format("({} {} {})", op_a, func, op_b);

        return ApplyPrecise(operation, std::move(op_str), result_type);
    }

    Expression GenerateBinaryCall(Operation operation, std::string_view func, Type result_type,
                                  Type type_a, Type type_b) {
        const std::string op_a = VisitOperand(operation, 0).As(type_a);
        const std::string op_b = VisitOperand(operation, 1).As(type_b);
        std::string op_str = fmt::format("{}({}, {})", func, op_a, op_b);

        return ApplyPrecise(operation, std::move(op_str), result_type);
    }

    Expression GenerateTernary(Operation operation, std::string_view func, Type result_type,
                               Type type_a, Type type_b, Type type_c) {
        const std::string op_a = VisitOperand(operation, 0).As(type_a);
        const std::string op_b = VisitOperand(operation, 1).As(type_b);
        const std::string op_c = VisitOperand(operation, 2).As(type_c);
        std::string op_str = fmt::format("{}({}, {}, {})", func, op_a, op_b, op_c);

        return ApplyPrecise(operation, std::move(op_str), result_type);
    }

    Expression GenerateQuaternary(Operation operation, const std::string& func, Type result_type,
                                  Type type_a, Type type_b, Type type_c, Type type_d) {
        const std::string op_a = VisitOperand(operation, 0).As(type_a);
        const std::string op_b = VisitOperand(operation, 1).As(type_b);
        const std::string op_c = VisitOperand(operation, 2).As(type_c);
        const std::string op_d = VisitOperand(operation, 3).As(type_d);
        std::string op_str = fmt::format("{}({}, {}, {}, {})", func, op_a, op_b, op_c, op_d);

        return ApplyPrecise(operation, std::move(op_str), result_type);
    }

    std::string GenerateTexture(Operation operation, const std::string& function_suffix,
                                const std::vector<TextureIR>& extras, bool separate_dc = false) {
        constexpr std::array coord_constructors = {"float", "vec2", "vec3", "vec4"};

        const auto meta = std::get_if<MetaTexture>(&operation.GetMeta());
        ASSERT(meta);

        const std::size_t count = operation.GetOperandsCount();
        const bool has_array = meta->sampler.IsArray();
        const bool has_shadow = meta->sampler.IsShadow();

        std::string expr = "texture" + function_suffix;
        if (!meta->aoffi.empty()) {
            expr += "Offset";
        } else if (!meta->ptp.empty()) {
            expr += "Offsets";
        }
        if (!meta->sampler.IsIndexed()) {
            expr += '(' + GetSampler(meta->sampler) + ", ";
        } else {
            expr += '(' + GetSampler(meta->sampler) + '[' + Visit(meta->index).AsUint() + "], ";
        }
        expr += coord_constructors.at(count + (has_array ? 1 : 0) +
                                      (has_shadow && !separate_dc ? 1 : 0) - 1);
        expr += '(';
        for (std::size_t i = 0; i < count; ++i) {
            expr += Visit(operation[i]).AsFloat();

            const std::size_t next = i + 1;
            if (next < count)
                expr += ", ";
        }
        if (has_array) {
            expr += ", float(" + Visit(meta->array).AsInt() + ')';
        }
        if (has_shadow) {
            if (separate_dc) {
                expr += "), " + Visit(meta->depth_compare).AsFloat();
            } else {
                expr += ", " + Visit(meta->depth_compare).AsFloat() + ')';
            }
        } else {
            expr += ')';
        }

        for (const auto& variant : extras) {
            if (const auto argument = std::get_if<TextureArgument>(&variant)) {
                expr += GenerateTextureArgument(*argument);
            } else if (std::holds_alternative<TextureOffset>(variant)) {
                if (!meta->aoffi.empty()) {
                    expr += GenerateTextureAoffi(meta->aoffi);
                } else if (!meta->ptp.empty()) {
                    expr += GenerateTexturePtp(meta->ptp);
                }
            } else if (std::holds_alternative<TextureDerivates>(variant)) {
                expr += GenerateTextureDerivates(meta->derivates);
            } else {
                UNREACHABLE();
            }
        }

        return expr + ')';
    }

    std::string GenerateTextureArgument(const TextureArgument& argument) {
        const auto& [type, operand] = argument;
        if (operand == nullptr) {
            return {};
        }

        std::string expr = ", ";
        switch (type) {
        case Type::Int:
            if (const auto immediate = std::get_if<ImmediateNode>(&*operand)) {
                // Inline the string as an immediate integer in GLSL (some extra arguments are
                // required to be constant)
                expr += std::to_string(static_cast<s32>(immediate->GetValue()));
            } else {
                expr += Visit(operand).AsInt();
            }
            break;
        case Type::Float:
            expr += Visit(operand).AsFloat();
            break;
        default: {
            const auto type_int = static_cast<u32>(type);
            UNIMPLEMENTED_MSG("Unimplemented extra type={}", type_int);
            expr += '0';
            break;
        }
        }
        return expr;
    }

    std::string ReadTextureOffset(const Node& value) {
        if (const auto immediate = std::get_if<ImmediateNode>(&*value)) {
            // Inline the string as an immediate integer in GLSL (AOFFI arguments are required
            // to be constant by the standard).
            return std::to_string(static_cast<s32>(immediate->GetValue()));
        } else if (device.HasVariableAoffi()) {
            // Avoid using variable AOFFI on unsupported devices.
            return Visit(value).AsInt();
        } else {
            // Insert 0 on devices not supporting variable AOFFI.
            return "0";
        }
    }

    std::string GenerateTextureAoffi(const std::vector<Node>& aoffi) {
        if (aoffi.empty()) {
            return {};
        }
        constexpr std::array coord_constructors = {"int", "ivec2", "ivec3"};
        std::string expr = ", ";
        expr += coord_constructors.at(aoffi.size() - 1);
        expr += '(';

        for (std::size_t index = 0; index < aoffi.size(); ++index) {
            expr += ReadTextureOffset(aoffi.at(index));
            if (index + 1 < aoffi.size()) {
                expr += ", ";
            }
        }
        expr += ')';

        return expr;
    }

    std::string GenerateTexturePtp(const std::vector<Node>& ptp) {
        static constexpr std::size_t num_vectors = 4;
        ASSERT(ptp.size() == num_vectors * 2);

        std::string expr = ", ivec2[](";
        for (std::size_t vector = 0; vector < num_vectors; ++vector) {
            const bool has_next = vector + 1 < num_vectors;
            expr += fmt::format("ivec2({}, {}){}", ReadTextureOffset(ptp.at(vector * 2)),
                                ReadTextureOffset(ptp.at(vector * 2 + 1)), has_next ? ", " : "");
        }
        expr += ')';
        return expr;
    }

    std::string GenerateTextureDerivates(const std::vector<Node>& derivates) {
        if (derivates.empty()) {
            return {};
        }
        constexpr std::array coord_constructors = {"float", "vec2", "vec3"};
        std::string expr = ", ";
        const std::size_t components = derivates.size() / 2;
        std::string dx = coord_constructors.at(components - 1);
        std::string dy = coord_constructors.at(components - 1);
        dx += '(';
        dy += '(';

        for (std::size_t index = 0; index < components; ++index) {
            const auto operand_x{derivates.at(index * 2)};
            const auto operand_y{derivates.at(index * 2 + 1)};
            dx += Visit(operand_x).AsFloat();
            dy += Visit(operand_y).AsFloat();

            if (index + 1 < components) {
                dx += ", ";
                dy += ", ";
            }
        }
        dx += ')';
        dy += ')';
        expr += dx + ", " + dy;

        return expr;
    }

    std::string BuildIntegerCoordinates(Operation operation) {
        constexpr std::array constructors{"int(", "ivec2(", "ivec3(", "ivec4("};
        const std::size_t coords_count{operation.GetOperandsCount()};
        std::string expr = constructors.at(coords_count - 1);
        for (std::size_t i = 0; i < coords_count; ++i) {
            expr += VisitOperand(operation, i).AsInt();
            if (i + 1 < coords_count) {
                expr += ", ";
            }
        }
        expr += ')';
        return expr;
    }

    std::string BuildImageValues(Operation operation) {
        constexpr std::array constructors{"uint", "uvec2", "uvec3", "uvec4"};
        const auto& meta{std::get<MetaImage>(operation.GetMeta())};

        const std::size_t values_count{meta.values.size()};
        std::string expr = fmt::format("{}(", constructors.at(values_count - 1));
        for (std::size_t i = 0; i < values_count; ++i) {
            expr += Visit(meta.values.at(i)).AsUint();
            if (i + 1 < values_count) {
                expr += ", ";
            }
        }
        expr += ')';
        return expr;
    }

    Expression Assign(Operation operation) {
        const Node& dest = operation[0];
        const Node& src = operation[1];

        Expression target;
        if (const auto gpr = std::get_if<GprNode>(&*dest)) {
            if (gpr->GetIndex() == Register::ZeroIndex) {
                // Writing to Register::ZeroIndex is a no op
                return {};
            }
            target = {GetRegister(gpr->GetIndex()), Type::Float};
        } else if (const auto abuf = std::get_if<AbufNode>(&*dest)) {
            UNIMPLEMENTED_IF(abuf->IsPhysicalBuffer());
            auto output = GetOutputAttribute(abuf);
            if (!output) {
                return {};
            }
            target = std::move(*output);
        } else if (const auto lmem = std::get_if<LmemNode>(&*dest)) {
            target = {
                fmt::format("{}[{} >> 2]", GetLocalMemory(), Visit(lmem->GetAddress()).AsUint()),
                Type::Uint};
        } else if (const auto smem = std::get_if<SmemNode>(&*dest)) {
            ASSERT(stage == ShaderType::Compute);
            target = {fmt::format("smem[{} >> 2]", Visit(smem->GetAddress()).AsUint()), Type::Uint};
        } else if (const auto gmem = std::get_if<GmemNode>(&*dest)) {
            const std::string real = Visit(gmem->GetRealAddress()).AsUint();
            const std::string base = Visit(gmem->GetBaseAddress()).AsUint();
            const std::string final_offset = fmt::format("({} - {}) >> 2", real, base);
            target = {fmt::format("{}[{}]", GetGlobalMemory(gmem->GetDescriptor()), final_offset),
                      Type::Uint};
        } else if (const auto cv = std::get_if<CustomVarNode>(&*dest)) {
            target = {GetCustomVariable(cv->GetIndex()), Type::Float};
        } else {
            UNREACHABLE_MSG("Assign called without a proper target");
        }

        code.AddLine("{} = {};", target.GetCode(), Visit(src).As(target.GetType()));
        return {};
    }

    template <Type type>
    Expression Add(Operation operation) {
        return GenerateBinaryInfix(operation, "+", type, type, type);
    }

    template <Type type>
    Expression Mul(Operation operation) {
        return GenerateBinaryInfix(operation, "*", type, type, type);
    }

    template <Type type>
    Expression Div(Operation operation) {
        return GenerateBinaryInfix(operation, "/", type, type, type);
    }

    template <Type type>
    Expression Fma(Operation operation) {
        return GenerateTernary(operation, "fma", type, type, type, type);
    }

    template <Type type>
    Expression Negate(Operation operation) {
        return GenerateUnary(operation, "-", type, type);
    }

    template <Type type>
    Expression Absolute(Operation operation) {
        return GenerateUnary(operation, "abs", type, type);
    }

    Expression FClamp(Operation operation) {
        return GenerateTernary(operation, "clamp", Type::Float, Type::Float, Type::Float,
                               Type::Float);
    }

    Expression FCastHalf0(Operation operation) {
        return {fmt::format("({})[0]", VisitOperand(operation, 0).AsHalfFloat()), Type::Float};
    }

    Expression FCastHalf1(Operation operation) {
        return {fmt::format("({})[1]", VisitOperand(operation, 0).AsHalfFloat()), Type::Float};
    }

    template <Type type>
    Expression Min(Operation operation) {
        return GenerateBinaryCall(operation, "min", type, type, type);
    }

    template <Type type>
    Expression Max(Operation operation) {
        return GenerateBinaryCall(operation, "max", type, type, type);
    }

    Expression Select(Operation operation) {
        const std::string condition = Visit(operation[0]).AsBool();
        const std::string true_case = Visit(operation[1]).AsUint();
        const std::string false_case = Visit(operation[2]).AsUint();
        std::string op_str = fmt::format("({} ? {} : {})", condition, true_case, false_case);

        return ApplyPrecise(operation, std::move(op_str), Type::Uint);
    }

    Expression FCos(Operation operation) {
        return GenerateUnary(operation, "cos", Type::Float, Type::Float);
    }

    Expression FSin(Operation operation) {
        return GenerateUnary(operation, "sin", Type::Float, Type::Float);
    }

    Expression FExp2(Operation operation) {
        return GenerateUnary(operation, "exp2", Type::Float, Type::Float);
    }

    Expression FLog2(Operation operation) {
        return GenerateUnary(operation, "log2", Type::Float, Type::Float);
    }

    Expression FInverseSqrt(Operation operation) {
        return GenerateUnary(operation, "inversesqrt", Type::Float, Type::Float);
    }

    Expression FSqrt(Operation operation) {
        return GenerateUnary(operation, "sqrt", Type::Float, Type::Float);
    }

    Expression FRoundEven(Operation operation) {
        return GenerateUnary(operation, "roundEven", Type::Float, Type::Float);
    }

    Expression FFloor(Operation operation) {
        return GenerateUnary(operation, "floor", Type::Float, Type::Float);
    }

    Expression FCeil(Operation operation) {
        return GenerateUnary(operation, "ceil", Type::Float, Type::Float);
    }

    Expression FTrunc(Operation operation) {
        return GenerateUnary(operation, "trunc", Type::Float, Type::Float);
    }

    template <Type type>
    Expression FCastInteger(Operation operation) {
        return GenerateUnary(operation, "float", Type::Float, type);
    }

    Expression FSwizzleAdd(Operation operation) {
        const std::string op_a = VisitOperand(operation, 0).AsFloat();
        const std::string op_b = VisitOperand(operation, 1).AsFloat();

        if (!device.HasShaderBallot()) {
            LOG_ERROR(Render_OpenGL, "Shader ballot is unavailable but required by the shader");
            return {fmt::format("{} + {}", op_a, op_b), Type::Float};
        }

        const std::string instr_mask = VisitOperand(operation, 2).AsUint();
        const std::string mask = code.GenerateTemporary();
        code.AddLine("uint {} = ({} >> ((gl_SubGroupInvocationARB & 3) << 1)) & 3;", mask,
                     instr_mask);

        const std::string modifier_a = fmt::format("fswzadd_modifiers_a[{}]", mask);
        const std::string modifier_b = fmt::format("fswzadd_modifiers_b[{}]", mask);
        return {fmt::format("(({} * {}) + ({} * {}))", op_a, modifier_a, op_b, modifier_b),
                Type::Float};
    }

    Expression ICastFloat(Operation operation) {
        return GenerateUnary(operation, "int", Type::Int, Type::Float);
    }

    Expression ICastUnsigned(Operation operation) {
        return GenerateUnary(operation, "int", Type::Int, Type::Uint);
    }

    template <Type type>
    Expression LogicalShiftLeft(Operation operation) {
        return GenerateBinaryInfix(operation, "<<", type, type, Type::Uint);
    }

    Expression ILogicalShiftRight(Operation operation) {
        const std::string op_a = VisitOperand(operation, 0).AsUint();
        const std::string op_b = VisitOperand(operation, 1).AsUint();
        std::string op_str = fmt::format("int({} >> {})", op_a, op_b);

        return ApplyPrecise(operation, std::move(op_str), Type::Int);
    }

    Expression IArithmeticShiftRight(Operation operation) {
        return GenerateBinaryInfix(operation, ">>", Type::Int, Type::Int, Type::Uint);
    }

    template <Type type>
    Expression BitwiseAnd(Operation operation) {
        return GenerateBinaryInfix(operation, "&", type, type, type);
    }

    template <Type type>
    Expression BitwiseOr(Operation operation) {
        return GenerateBinaryInfix(operation, "|", type, type, type);
    }

    template <Type type>
    Expression BitwiseXor(Operation operation) {
        return GenerateBinaryInfix(operation, "^", type, type, type);
    }

    template <Type type>
    Expression BitwiseNot(Operation operation) {
        return GenerateUnary(operation, "~", type, type);
    }

    Expression UCastFloat(Operation operation) {
        return GenerateUnary(operation, "uint", Type::Uint, Type::Float);
    }

    Expression UCastSigned(Operation operation) {
        return GenerateUnary(operation, "uint", Type::Uint, Type::Int);
    }

    Expression UShiftRight(Operation operation) {
        return GenerateBinaryInfix(operation, ">>", Type::Uint, Type::Uint, Type::Uint);
    }

    template <Type type>
    Expression BitfieldInsert(Operation operation) {
        return GenerateQuaternary(operation, "bitfieldInsert", type, type, type, Type::Int,
                                  Type::Int);
    }

    template <Type type>
    Expression BitfieldExtract(Operation operation) {
        return GenerateTernary(operation, "bitfieldExtract", type, type, Type::Int, Type::Int);
    }

    template <Type type>
    Expression BitCount(Operation operation) {
        return GenerateUnary(operation, "bitCount", type, type);
    }

    template <Type type>
    Expression BitMSB(Operation operation) {
        return GenerateUnary(operation, "findMSB", type, type);
    }

    Expression HNegate(Operation operation) {
        const auto GetNegate = [&](std::size_t index) {
            return VisitOperand(operation, index).AsBool() + " ? -1 : 1";
        };
        return {fmt::format("({} * vec2({}, {}))", VisitOperand(operation, 0).AsHalfFloat(),
                            GetNegate(1), GetNegate(2)),
                Type::HalfFloat};
    }

    Expression HClamp(Operation operation) {
        const std::string value = VisitOperand(operation, 0).AsHalfFloat();
        const std::string min = VisitOperand(operation, 1).AsFloat();
        const std::string max = VisitOperand(operation, 2).AsFloat();
        std::string clamped = fmt::format("clamp({}, vec2({}), vec2({}))", value, min, max);

        return ApplyPrecise(operation, std::move(clamped), Type::HalfFloat);
    }

    Expression HCastFloat(Operation operation) {
        return {fmt::format("vec2({}, 0.0f)", VisitOperand(operation, 0).AsFloat()),
                Type::HalfFloat};
    }

    Expression HUnpack(Operation operation) {
        Expression operand = VisitOperand(operation, 0);
        switch (std::get<Tegra::Shader::HalfType>(operation.GetMeta())) {
        case Tegra::Shader::HalfType::H0_H1:
            return operand;
        case Tegra::Shader::HalfType::F32:
            return {fmt::format("vec2({})", operand.AsFloat()), Type::HalfFloat};
        case Tegra::Shader::HalfType::H0_H0:
            return {fmt::format("vec2({}[0])", operand.AsHalfFloat()), Type::HalfFloat};
        case Tegra::Shader::HalfType::H1_H1:
            return {fmt::format("vec2({}[1])", operand.AsHalfFloat()), Type::HalfFloat};
        }
        UNREACHABLE();
        return {"0", Type::Int};
    }

    Expression HMergeF32(Operation operation) {
        return {fmt::format("float({}[0])", VisitOperand(operation, 0).AsHalfFloat()), Type::Float};
    }

    Expression HMergeH0(Operation operation) {
        std::string dest = VisitOperand(operation, 0).AsUint();
        std::string src = VisitOperand(operation, 1).AsUint();
        return {fmt::format("(({} & 0x0000FFFFU) | ({} & 0xFFFF0000U))", src, dest), Type::Uint};
    }

    Expression HMergeH1(Operation operation) {
        std::string dest = VisitOperand(operation, 0).AsUint();
        std::string src = VisitOperand(operation, 1).AsUint();
        return {fmt::format("(({} & 0x0000FFFFU) | ({} & 0xFFFF0000U))", dest, src), Type::Uint};
    }

    Expression HPack2(Operation operation) {
        return {fmt::format("vec2({}, {})", VisitOperand(operation, 0).AsFloat(),
                            VisitOperand(operation, 1).AsFloat()),
                Type::HalfFloat};
    }

    template <Type type>
    Expression LogicalLessThan(Operation operation) {
        return GenerateBinaryInfix(operation, "<", Type::Bool, type, type);
    }

    template <Type type>
    Expression LogicalEqual(Operation operation) {
        return GenerateBinaryInfix(operation, "==", Type::Bool, type, type);
    }

    template <Type type>
    Expression LogicalLessEqual(Operation operation) {
        return GenerateBinaryInfix(operation, "<=", Type::Bool, type, type);
    }

    template <Type type>
    Expression LogicalGreaterThan(Operation operation) {
        return GenerateBinaryInfix(operation, ">", Type::Bool, type, type);
    }

    template <Type type>
    Expression LogicalNotEqual(Operation operation) {
        return GenerateBinaryInfix(operation, "!=", Type::Bool, type, type);
    }

    template <Type type>
    Expression LogicalGreaterEqual(Operation operation) {
        return GenerateBinaryInfix(operation, ">=", Type::Bool, type, type);
    }

    Expression LogicalFIsNan(Operation operation) {
        return GenerateUnary(operation, "isnan", Type::Bool, Type::Float);
    }

    Expression LogicalAssign(Operation operation) {
        const Node& dest = operation[0];
        const Node& src = operation[1];

        std::string target;

        if (const auto pred = std::get_if<PredicateNode>(&*dest)) {
            ASSERT_MSG(!pred->IsNegated(), "Negating logical assignment");

            const auto index = pred->GetIndex();
            switch (index) {
            case Tegra::Shader::Pred::NeverExecute:
            case Tegra::Shader::Pred::UnusedIndex:
                // Writing to these predicates is a no-op
                return {};
            }
            target = GetPredicate(index);
        } else if (const auto flag = std::get_if<InternalFlagNode>(&*dest)) {
            target = GetInternalFlag(flag->GetFlag());
        }

        code.AddLine("{} = {};", target, Visit(src).AsBool());
        return {};
    }

    Expression LogicalAnd(Operation operation) {
        return GenerateBinaryInfix(operation, "&&", Type::Bool, Type::Bool, Type::Bool);
    }

    Expression LogicalOr(Operation operation) {
        return GenerateBinaryInfix(operation, "||", Type::Bool, Type::Bool, Type::Bool);
    }

    Expression LogicalXor(Operation operation) {
        return GenerateBinaryInfix(operation, "^^", Type::Bool, Type::Bool, Type::Bool);
    }

    Expression LogicalNegate(Operation operation) {
        return GenerateUnary(operation, "!", Type::Bool, Type::Bool);
    }

    Expression LogicalPick2(Operation operation) {
        return {fmt::format("{}[{}]", VisitOperand(operation, 0).AsBool2(),
                            VisitOperand(operation, 1).AsUint()),
                Type::Bool};
    }

    Expression LogicalAnd2(Operation operation) {
        return GenerateUnary(operation, "all", Type::Bool, Type::Bool2);
    }

    template <bool with_nan>
    Expression GenerateHalfComparison(Operation operation, std::string_view compare_op) {
        Expression comparison = GenerateBinaryCall(operation, compare_op, Type::Bool2,
                                                   Type::HalfFloat, Type::HalfFloat);
        if constexpr (!with_nan) {
            return comparison;
        }
        return {fmt::format("HalfFloatNanComparison({}, {}, {})", comparison.AsBool2(),
                            VisitOperand(operation, 0).AsHalfFloat(),
                            VisitOperand(operation, 1).AsHalfFloat()),
                Type::Bool2};
    }

    template <bool with_nan>
    Expression Logical2HLessThan(Operation operation) {
        return GenerateHalfComparison<with_nan>(operation, "lessThan");
    }

    template <bool with_nan>
    Expression Logical2HEqual(Operation operation) {
        return GenerateHalfComparison<with_nan>(operation, "equal");
    }

    template <bool with_nan>
    Expression Logical2HLessEqual(Operation operation) {
        return GenerateHalfComparison<with_nan>(operation, "lessThanEqual");
    }

    template <bool with_nan>
    Expression Logical2HGreaterThan(Operation operation) {
        return GenerateHalfComparison<with_nan>(operation, "greaterThan");
    }

    template <bool with_nan>
    Expression Logical2HNotEqual(Operation operation) {
        return GenerateHalfComparison<with_nan>(operation, "notEqual");
    }

    template <bool with_nan>
    Expression Logical2HGreaterEqual(Operation operation) {
        return GenerateHalfComparison<with_nan>(operation, "greaterThanEqual");
    }

    Expression Texture(Operation operation) {
        const auto meta = std::get_if<MetaTexture>(&operation.GetMeta());
        ASSERT(meta);

        std::string expr = GenerateTexture(
            operation, "", {TextureOffset{}, TextureArgument{Type::Float, meta->bias}});
        if (meta->sampler.IsShadow()) {
            expr = "vec4(" + expr + ')';
        }
        return {expr + GetSwizzle(meta->element), Type::Float};
    }

    Expression TextureLod(Operation operation) {
        const auto meta = std::get_if<MetaTexture>(&operation.GetMeta());
        ASSERT(meta);

        std::string expr = GenerateTexture(
            operation, "Lod", {TextureArgument{Type::Float, meta->lod}, TextureOffset{}});
        if (meta->sampler.IsShadow()) {
            expr = "vec4(" + expr + ')';
        }
        return {expr + GetSwizzle(meta->element), Type::Float};
    }

    Expression TextureGather(Operation operation) {
        const auto& meta = std::get<MetaTexture>(operation.GetMeta());

        const auto type = meta.sampler.IsShadow() ? Type::Float : Type::Int;
        const bool separate_dc = meta.sampler.IsShadow();

        std::vector<TextureIR> ir;
        if (meta.sampler.IsShadow()) {
            ir = {TextureOffset{}};
        } else {
            ir = {TextureOffset{}, TextureArgument{type, meta.component}};
        }
        return {GenerateTexture(operation, "Gather", ir, separate_dc) + GetSwizzle(meta.element),
                Type::Float};
    }

    Expression TextureQueryDimensions(Operation operation) {
        const auto meta = std::get_if<MetaTexture>(&operation.GetMeta());
        ASSERT(meta);

        const std::string sampler = GetSampler(meta->sampler);
        const std::string lod = VisitOperand(operation, 0).AsInt();

        switch (meta->element) {
        case 0:
        case 1:
            return {fmt::format("textureSize({}, {}){}", sampler, lod, GetSwizzle(meta->element)),
                    Type::Int};
        case 3:
            return {fmt::format("textureQueryLevels({})", sampler), Type::Int};
        }
        UNREACHABLE();
        return {"0", Type::Int};
    }

    Expression TextureQueryLod(Operation operation) {
        const auto meta = std::get_if<MetaTexture>(&operation.GetMeta());
        ASSERT(meta);

        if (meta->element < 2) {
            return {fmt::format("int(({} * vec2(256)){})",
                                GenerateTexture(operation, "QueryLod", {}),
                                GetSwizzle(meta->element)),
                    Type::Int};
        }
        return {"0", Type::Int};
    }

    Expression TexelFetch(Operation operation) {
        constexpr std::array constructors = {"int", "ivec2", "ivec3", "ivec4"};
        const auto meta = std::get_if<MetaTexture>(&operation.GetMeta());
        ASSERT(meta);
        UNIMPLEMENTED_IF(meta->sampler.IsArray());
        const std::size_t count = operation.GetOperandsCount();

        std::string expr = "texelFetch(";
        expr += GetSampler(meta->sampler);
        expr += ", ";

        expr += constructors.at(operation.GetOperandsCount() - 1);
        expr += '(';
        for (std::size_t i = 0; i < count; ++i) {
            expr += VisitOperand(operation, i).AsInt();
            const std::size_t next = i + 1;
            if (next == count)
                expr += ')';
            else if (next < count)
                expr += ", ";
        }

        if (meta->lod && !meta->sampler.IsBuffer()) {
            expr += ", ";
            expr += Visit(meta->lod).AsInt();
        }
        expr += ')';
        expr += GetSwizzle(meta->element);

        return {std::move(expr), Type::Float};
    }

    Expression TextureGradient(Operation operation) {
        const auto meta = std::get_if<MetaTexture>(&operation.GetMeta());
        ASSERT(meta);

        std::string expr =
            GenerateTexture(operation, "Grad", {TextureDerivates{}, TextureOffset{}});
        return {std::move(expr) + GetSwizzle(meta->element), Type::Float};
    }

    Expression ImageLoad(Operation operation) {
        if (!device.HasImageLoadFormatted()) {
            LOG_ERROR(Render_OpenGL,
                      "Device lacks GL_EXT_shader_image_load_formatted, stubbing image load");
            return {"0", Type::Int};
        }

        const auto& meta{std::get<MetaImage>(operation.GetMeta())};
        return {fmt::format("imageLoad({}, {}){}", GetImage(meta.image),
                            BuildIntegerCoordinates(operation), GetSwizzle(meta.element)),
                Type::Uint};
    }

    Expression ImageStore(Operation operation) {
        const auto& meta{std::get<MetaImage>(operation.GetMeta())};
        code.AddLine("imageStore({}, {}, {});", GetImage(meta.image),
                     BuildIntegerCoordinates(operation), BuildImageValues(operation));
        return {};
    }

    template <const std::string_view& opname>
    Expression AtomicImage(Operation operation) {
        const auto& meta{std::get<MetaImage>(operation.GetMeta())};
        ASSERT(meta.values.size() == 1);

        return {fmt::format("imageAtomic{}({}, {}, {})", opname, GetImage(meta.image),
                            BuildIntegerCoordinates(operation), Visit(meta.values[0]).AsUint()),
                Type::Uint};
    }

    template <const std::string_view& opname, Type type>
    Expression Atomic(Operation operation) {
        return {fmt::format("atomic{}({}, {})", opname, Visit(operation[0]).GetCode(),
                            Visit(operation[1]).As(type)),
                type};
    }

    Expression Branch(Operation operation) {
        const auto target = std::get_if<ImmediateNode>(&*operation[0]);
        UNIMPLEMENTED_IF(!target);

        code.AddLine("jmp_to = 0x{:X}U;", target->GetValue());
        code.AddLine("break;");
        return {};
    }

    Expression BranchIndirect(Operation operation) {
        const std::string op_a = VisitOperand(operation, 0).AsUint();

        code.AddLine("jmp_to = {};", op_a);
        code.AddLine("break;");
        return {};
    }

    Expression PushFlowStack(Operation operation) {
        const auto stack = std::get<MetaStackClass>(operation.GetMeta());
        const auto target = std::get_if<ImmediateNode>(&*operation[0]);
        UNIMPLEMENTED_IF(!target);

        code.AddLine("{}[{}++] = 0x{:X}U;", FlowStackName(stack), FlowStackTopName(stack),
                     target->GetValue());
        return {};
    }

    Expression PopFlowStack(Operation operation) {
        const auto stack = std::get<MetaStackClass>(operation.GetMeta());
        code.AddLine("jmp_to = {}[--{}];", FlowStackName(stack), FlowStackTopName(stack));
        code.AddLine("break;");
        return {};
    }

    void PreExit() {
        if (stage != ShaderType::Fragment) {
            return;
        }
        const auto& used_registers = ir.GetRegisters();
        const auto SafeGetRegister = [&](u32 reg) -> Expression {
            // TODO(Rodrigo): Replace with contains once C++20 releases
            if (used_registers.find(reg) != used_registers.end()) {
                return {GetRegister(reg), Type::Float};
            }
            return {"0.0f", Type::Float};
        };

        UNIMPLEMENTED_IF_MSG(header.ps.omap.sample_mask != 0, "Sample mask write is unimplemented");

        // Write the color outputs using the data in the shader registers, disabled
        // rendertargets/components are skipped in the register assignment.
        u32 current_reg = 0;
        for (u32 render_target = 0; render_target < Maxwell::NumRenderTargets; ++render_target) {
            // TODO(Subv): Figure out how dual-source blending is configured in the Switch.
            for (u32 component = 0; component < 4; ++component) {
                if (header.ps.IsColorComponentOutputEnabled(render_target, component)) {
                    code.AddLine("FragColor{}[{}] = {};", render_target, component,
                                 SafeGetRegister(current_reg).AsFloat());
                    ++current_reg;
                }
            }
        }

        if (header.ps.omap.depth) {
            // The depth output is always 2 registers after the last color output, and current_reg
            // already contains one past the last color register.
            code.AddLine("gl_FragDepth = {};", SafeGetRegister(current_reg + 1).AsFloat());
        }
    }

    Expression Exit(Operation operation) {
        PreExit();
        code.AddLine("return;");
        return {};
    }

    Expression Discard(Operation operation) {
        // Enclose "discard" in a conditional, so that GLSL compilation does not complain
        // about unexecuted instructions that may follow this.
        code.AddLine("if (true) {{");
        ++code.scope;
        code.AddLine("discard;");
        --code.scope;
        code.AddLine("}}");
        return {};
    }

    Expression EmitVertex(Operation operation) {
        ASSERT_MSG(stage == ShaderType::Geometry,
                   "EmitVertex is expected to be used in a geometry shader.");
        code.AddLine("EmitVertex();");
        return {};
    }

    Expression EndPrimitive(Operation operation) {
        ASSERT_MSG(stage == ShaderType::Geometry,
                   "EndPrimitive is expected to be used in a geometry shader.");
        code.AddLine("EndPrimitive();");
        return {};
    }

    Expression InvocationId(Operation operation) {
        return {"gl_InvocationID", Type::Int};
    }

    Expression YNegate(Operation operation) {
        return {"y_direction", Type::Float};
    }

    template <u32 element>
    Expression LocalInvocationId(Operation) {
        return {"gl_LocalInvocationID"s + GetSwizzle(element), Type::Uint};
    }

    template <u32 element>
    Expression WorkGroupId(Operation) {
        return {"gl_WorkGroupID"s + GetSwizzle(element), Type::Uint};
    }

    Expression BallotThread(Operation operation) {
        const std::string value = VisitOperand(operation, 0).AsBool();
        if (!device.HasWarpIntrinsics()) {
            LOG_ERROR(Render_OpenGL, "Nvidia vote intrinsics are required by this shader");
            // Stub on non-Nvidia devices by simulating all threads voting the same as the active
            // one.
            return {fmt::format("({} ? 0xFFFFFFFFU : 0U)", value), Type::Uint};
        }
        return {fmt::format("ballotThreadNV({})", value), Type::Uint};
    }

    Expression Vote(Operation operation, const char* func) {
        const std::string value = VisitOperand(operation, 0).AsBool();
        if (!device.HasWarpIntrinsics()) {
            LOG_ERROR(Render_OpenGL, "Nvidia vote intrinsics are required by this shader");
            // Stub with a warp size of one.
            return {value, Type::Bool};
        }
        return {fmt::format("{}({})", func, value), Type::Bool};
    }

    Expression VoteAll(Operation operation) {
        return Vote(operation, "allThreadsNV");
    }

    Expression VoteAny(Operation operation) {
        return Vote(operation, "anyThreadNV");
    }

    Expression VoteEqual(Operation operation) {
        if (!device.HasWarpIntrinsics()) {
            LOG_ERROR(Render_OpenGL, "Nvidia vote intrinsics are required by this shader");
            // We must return true here since a stub for a theoretical warp size of 1.
            // This will always return an equal result across all votes.
            return {"true", Type::Bool};
        }
        return Vote(operation, "allThreadsEqualNV");
    }

    Expression ThreadId(Operation operation) {
        if (!device.HasShaderBallot()) {
            LOG_ERROR(Render_OpenGL, "Shader ballot is unavailable but required by the shader");
            return {"0U", Type::Uint};
        }
        return {"gl_SubGroupInvocationARB", Type::Uint};
    }

    Expression ShuffleIndexed(Operation operation) {
        std::string value = VisitOperand(operation, 0).AsFloat();

        if (!device.HasShaderBallot()) {
            LOG_ERROR(Render_OpenGL, "Shader ballot is unavailable but required by the shader");
            return {std::move(value), Type::Float};
        }

        const std::string index = VisitOperand(operation, 1).AsUint();
        return {fmt::format("readInvocationARB({}, {})", value, index), Type::Float};
    }

    Expression MemoryBarrierGL(Operation) {
        code.AddLine("memoryBarrier();");
        return {};
    }

    struct Func final {
        Func() = delete;
        ~Func() = delete;

        static constexpr std::string_view Add = "Add";
        static constexpr std::string_view And = "And";
        static constexpr std::string_view Or = "Or";
        static constexpr std::string_view Xor = "Xor";
        static constexpr std::string_view Exchange = "Exchange";
    };

    static constexpr std::array operation_decompilers = {
        &GLSLDecompiler::Assign,

        &GLSLDecompiler::Select,

        &GLSLDecompiler::Add<Type::Float>,
        &GLSLDecompiler::Mul<Type::Float>,
        &GLSLDecompiler::Div<Type::Float>,
        &GLSLDecompiler::Fma<Type::Float>,
        &GLSLDecompiler::Negate<Type::Float>,
        &GLSLDecompiler::Absolute<Type::Float>,
        &GLSLDecompiler::FClamp,
        &GLSLDecompiler::FCastHalf0,
        &GLSLDecompiler::FCastHalf1,
        &GLSLDecompiler::Min<Type::Float>,
        &GLSLDecompiler::Max<Type::Float>,
        &GLSLDecompiler::FCos,
        &GLSLDecompiler::FSin,
        &GLSLDecompiler::FExp2,
        &GLSLDecompiler::FLog2,
        &GLSLDecompiler::FInverseSqrt,
        &GLSLDecompiler::FSqrt,
        &GLSLDecompiler::FRoundEven,
        &GLSLDecompiler::FFloor,
        &GLSLDecompiler::FCeil,
        &GLSLDecompiler::FTrunc,
        &GLSLDecompiler::FCastInteger<Type::Int>,
        &GLSLDecompiler::FCastInteger<Type::Uint>,
        &GLSLDecompiler::FSwizzleAdd,

        &GLSLDecompiler::Add<Type::Int>,
        &GLSLDecompiler::Mul<Type::Int>,
        &GLSLDecompiler::Div<Type::Int>,
        &GLSLDecompiler::Negate<Type::Int>,
        &GLSLDecompiler::Absolute<Type::Int>,
        &GLSLDecompiler::Min<Type::Int>,
        &GLSLDecompiler::Max<Type::Int>,

        &GLSLDecompiler::ICastFloat,
        &GLSLDecompiler::ICastUnsigned,
        &GLSLDecompiler::LogicalShiftLeft<Type::Int>,
        &GLSLDecompiler::ILogicalShiftRight,
        &GLSLDecompiler::IArithmeticShiftRight,
        &GLSLDecompiler::BitwiseAnd<Type::Int>,
        &GLSLDecompiler::BitwiseOr<Type::Int>,
        &GLSLDecompiler::BitwiseXor<Type::Int>,
        &GLSLDecompiler::BitwiseNot<Type::Int>,
        &GLSLDecompiler::BitfieldInsert<Type::Int>,
        &GLSLDecompiler::BitfieldExtract<Type::Int>,
        &GLSLDecompiler::BitCount<Type::Int>,
        &GLSLDecompiler::BitMSB<Type::Int>,

        &GLSLDecompiler::Add<Type::Uint>,
        &GLSLDecompiler::Mul<Type::Uint>,
        &GLSLDecompiler::Div<Type::Uint>,
        &GLSLDecompiler::Min<Type::Uint>,
        &GLSLDecompiler::Max<Type::Uint>,
        &GLSLDecompiler::UCastFloat,
        &GLSLDecompiler::UCastSigned,
        &GLSLDecompiler::LogicalShiftLeft<Type::Uint>,
        &GLSLDecompiler::UShiftRight,
        &GLSLDecompiler::UShiftRight,
        &GLSLDecompiler::BitwiseAnd<Type::Uint>,
        &GLSLDecompiler::BitwiseOr<Type::Uint>,
        &GLSLDecompiler::BitwiseXor<Type::Uint>,
        &GLSLDecompiler::BitwiseNot<Type::Uint>,
        &GLSLDecompiler::BitfieldInsert<Type::Uint>,
        &GLSLDecompiler::BitfieldExtract<Type::Uint>,
        &GLSLDecompiler::BitCount<Type::Uint>,
        &GLSLDecompiler::BitMSB<Type::Uint>,

        &GLSLDecompiler::Add<Type::HalfFloat>,
        &GLSLDecompiler::Mul<Type::HalfFloat>,
        &GLSLDecompiler::Fma<Type::HalfFloat>,
        &GLSLDecompiler::Absolute<Type::HalfFloat>,
        &GLSLDecompiler::HNegate,
        &GLSLDecompiler::HClamp,
        &GLSLDecompiler::HCastFloat,
        &GLSLDecompiler::HUnpack,
        &GLSLDecompiler::HMergeF32,
        &GLSLDecompiler::HMergeH0,
        &GLSLDecompiler::HMergeH1,
        &GLSLDecompiler::HPack2,

        &GLSLDecompiler::LogicalAssign,
        &GLSLDecompiler::LogicalAnd,
        &GLSLDecompiler::LogicalOr,
        &GLSLDecompiler::LogicalXor,
        &GLSLDecompiler::LogicalNegate,
        &GLSLDecompiler::LogicalPick2,
        &GLSLDecompiler::LogicalAnd2,

        &GLSLDecompiler::LogicalLessThan<Type::Float>,
        &GLSLDecompiler::LogicalEqual<Type::Float>,
        &GLSLDecompiler::LogicalLessEqual<Type::Float>,
        &GLSLDecompiler::LogicalGreaterThan<Type::Float>,
        &GLSLDecompiler::LogicalNotEqual<Type::Float>,
        &GLSLDecompiler::LogicalGreaterEqual<Type::Float>,
        &GLSLDecompiler::LogicalFIsNan,

        &GLSLDecompiler::LogicalLessThan<Type::Int>,
        &GLSLDecompiler::LogicalEqual<Type::Int>,
        &GLSLDecompiler::LogicalLessEqual<Type::Int>,
        &GLSLDecompiler::LogicalGreaterThan<Type::Int>,
        &GLSLDecompiler::LogicalNotEqual<Type::Int>,
        &GLSLDecompiler::LogicalGreaterEqual<Type::Int>,

        &GLSLDecompiler::LogicalLessThan<Type::Uint>,
        &GLSLDecompiler::LogicalEqual<Type::Uint>,
        &GLSLDecompiler::LogicalLessEqual<Type::Uint>,
        &GLSLDecompiler::LogicalGreaterThan<Type::Uint>,
        &GLSLDecompiler::LogicalNotEqual<Type::Uint>,
        &GLSLDecompiler::LogicalGreaterEqual<Type::Uint>,

        &GLSLDecompiler::Logical2HLessThan<false>,
        &GLSLDecompiler::Logical2HEqual<false>,
        &GLSLDecompiler::Logical2HLessEqual<false>,
        &GLSLDecompiler::Logical2HGreaterThan<false>,
        &GLSLDecompiler::Logical2HNotEqual<false>,
        &GLSLDecompiler::Logical2HGreaterEqual<false>,
        &GLSLDecompiler::Logical2HLessThan<true>,
        &GLSLDecompiler::Logical2HEqual<true>,
        &GLSLDecompiler::Logical2HLessEqual<true>,
        &GLSLDecompiler::Logical2HGreaterThan<true>,
        &GLSLDecompiler::Logical2HNotEqual<true>,
        &GLSLDecompiler::Logical2HGreaterEqual<true>,

        &GLSLDecompiler::Texture,
        &GLSLDecompiler::TextureLod,
        &GLSLDecompiler::TextureGather,
        &GLSLDecompiler::TextureQueryDimensions,
        &GLSLDecompiler::TextureQueryLod,
        &GLSLDecompiler::TexelFetch,
        &GLSLDecompiler::TextureGradient,

        &GLSLDecompiler::ImageLoad,
        &GLSLDecompiler::ImageStore,

        &GLSLDecompiler::AtomicImage<Func::Add>,
        &GLSLDecompiler::AtomicImage<Func::And>,
        &GLSLDecompiler::AtomicImage<Func::Or>,
        &GLSLDecompiler::AtomicImage<Func::Xor>,
        &GLSLDecompiler::AtomicImage<Func::Exchange>,

        &GLSLDecompiler::Atomic<Func::Add, Type::Uint>,

        &GLSLDecompiler::Branch,
        &GLSLDecompiler::BranchIndirect,
        &GLSLDecompiler::PushFlowStack,
        &GLSLDecompiler::PopFlowStack,
        &GLSLDecompiler::Exit,
        &GLSLDecompiler::Discard,

        &GLSLDecompiler::EmitVertex,
        &GLSLDecompiler::EndPrimitive,

        &GLSLDecompiler::InvocationId,
        &GLSLDecompiler::YNegate,
        &GLSLDecompiler::LocalInvocationId<0>,
        &GLSLDecompiler::LocalInvocationId<1>,
        &GLSLDecompiler::LocalInvocationId<2>,
        &GLSLDecompiler::WorkGroupId<0>,
        &GLSLDecompiler::WorkGroupId<1>,
        &GLSLDecompiler::WorkGroupId<2>,

        &GLSLDecompiler::BallotThread,
        &GLSLDecompiler::VoteAll,
        &GLSLDecompiler::VoteAny,
        &GLSLDecompiler::VoteEqual,

        &GLSLDecompiler::ThreadId,
        &GLSLDecompiler::ShuffleIndexed,

        &GLSLDecompiler::MemoryBarrierGL,
    };
    static_assert(operation_decompilers.size() == static_cast<std::size_t>(OperationCode::Amount));

    std::string GetRegister(u32 index) const {
        return GetDeclarationWithSuffix(index, "gpr");
    }

    std::string GetCustomVariable(u32 index) const {
        return GetDeclarationWithSuffix(index, "custom_var");
    }

    std::string GetPredicate(Tegra::Shader::Pred pred) const {
        return GetDeclarationWithSuffix(static_cast<u32>(pred), "pred");
    }

    std::string GetInputAttribute(Attribute::Index attribute) const {
        return GetDeclarationWithSuffix(GetGenericAttributeIndex(attribute), "input_attr");
    }

    std::string GetOutputAttribute(Attribute::Index attribute) const {
        return GetDeclarationWithSuffix(GetGenericAttributeIndex(attribute), "output_attr");
    }

    std::string GetConstBuffer(u32 index) const {
        return GetDeclarationWithSuffix(index, "cbuf");
    }

    std::string GetGlobalMemory(const GlobalMemoryBase& descriptor) const {
        return fmt::format("gmem_{}_{}_{}", descriptor.cbuf_index, descriptor.cbuf_offset, suffix);
    }

    std::string GetGlobalMemoryBlock(const GlobalMemoryBase& descriptor) const {
        return fmt::format("gmem_block_{}_{}_{}", descriptor.cbuf_index, descriptor.cbuf_offset,
                           suffix);
    }

    std::string GetConstBufferBlock(u32 index) const {
        return GetDeclarationWithSuffix(index, "cbuf_block");
    }

    std::string GetLocalMemory() const {
        return "lmem_" + suffix;
    }

    std::string GetInternalFlag(InternalFlag flag) const {
        constexpr std::array InternalFlagNames = {"zero_flag", "sign_flag", "carry_flag",
                                                  "overflow_flag"};
        const auto index = static_cast<u32>(flag);
        ASSERT(index < static_cast<u32>(InternalFlag::Amount));

        return fmt::format("{}_{}", InternalFlagNames[index], suffix);
    }

    std::string GetSampler(const Sampler& sampler) const {
        return GetDeclarationWithSuffix(static_cast<u32>(sampler.GetIndex()), "sampler");
    }

    std::string GetImage(const Image& image) const {
        return GetDeclarationWithSuffix(static_cast<u32>(image.GetIndex()), "image");
    }

    std::string GetDeclarationWithSuffix(u32 index, std::string_view name) const {
        return fmt::format("{}_{}_{}", name, index, suffix);
    }

    u32 GetNumPhysicalInputAttributes() const {
        return IsVertexShader(stage) ? GetNumPhysicalAttributes() : GetNumPhysicalVaryings();
    }

    u32 GetNumPhysicalAttributes() const {
        return std::min<u32>(device.GetMaxVertexAttributes(), Maxwell::NumVertexAttributes);
    }

    u32 GetNumPhysicalVaryings() const {
        return std::min<u32>(device.GetMaxVaryings(), Maxwell::NumVaryings);
    }

    const Device& device;
    const ShaderIR& ir;
    const ShaderType stage;
    const std::string suffix;
    const Header header;

    ShaderWriter code;
};

std::string GetFlowVariable(u32 i) {
    return fmt::format("flow_var_{}", i);
}

class ExprDecompiler {
public:
    explicit ExprDecompiler(GLSLDecompiler& decomp) : decomp{decomp} {}

    void operator()(const ExprAnd& expr) {
        inner += '(';
        std::visit(*this, *expr.operand1);
        inner += " && ";
        std::visit(*this, *expr.operand2);
        inner += ')';
    }

    void operator()(const ExprOr& expr) {
        inner += '(';
        std::visit(*this, *expr.operand1);
        inner += " || ";
        std::visit(*this, *expr.operand2);
        inner += ')';
    }

    void operator()(const ExprNot& expr) {
        inner += '!';
        std::visit(*this, *expr.operand1);
    }

    void operator()(const ExprPredicate& expr) {
        const auto pred = static_cast<Tegra::Shader::Pred>(expr.predicate);
        inner += decomp.GetPredicate(pred);
    }

    void operator()(const ExprCondCode& expr) {
        inner += decomp.Visit(decomp.ir.GetConditionCode(expr.cc)).AsBool();
    }

    void operator()(const ExprVar& expr) {
        inner += GetFlowVariable(expr.var_index);
    }

    void operator()(const ExprBoolean& expr) {
        inner += expr.value ? "true" : "false";
    }

    void operator()(VideoCommon::Shader::ExprGprEqual& expr) {
        inner += fmt::format("(ftou({}) == {})", decomp.GetRegister(expr.gpr), expr.value);
    }

    const std::string& GetResult() const {
        return inner;
    }

private:
    GLSLDecompiler& decomp;
    std::string inner;
};

class ASTDecompiler {
public:
    explicit ASTDecompiler(GLSLDecompiler& decomp) : decomp{decomp} {}

    void operator()(const ASTProgram& ast) {
        ASTNode current = ast.nodes.GetFirst();
        while (current) {
            Visit(current);
            current = current->GetNext();
        }
    }

    void operator()(const ASTIfThen& ast) {
        ExprDecompiler expr_parser{decomp};
        std::visit(expr_parser, *ast.condition);
        decomp.code.AddLine("if ({}) {{", expr_parser.GetResult());
        decomp.code.scope++;
        ASTNode current = ast.nodes.GetFirst();
        while (current) {
            Visit(current);
            current = current->GetNext();
        }
        decomp.code.scope--;
        decomp.code.AddLine("}}");
    }

    void operator()(const ASTIfElse& ast) {
        decomp.code.AddLine("else {{");
        decomp.code.scope++;
        ASTNode current = ast.nodes.GetFirst();
        while (current) {
            Visit(current);
            current = current->GetNext();
        }
        decomp.code.scope--;
        decomp.code.AddLine("}}");
    }

    void operator()([[maybe_unused]] const ASTBlockEncoded& ast) {
        UNREACHABLE();
    }

    void operator()(const ASTBlockDecoded& ast) {
        decomp.VisitBlock(ast.nodes);
    }

    void operator()(const ASTVarSet& ast) {
        ExprDecompiler expr_parser{decomp};
        std::visit(expr_parser, *ast.condition);
        decomp.code.AddLine("{} = {};", GetFlowVariable(ast.index), expr_parser.GetResult());
    }

    void operator()(const ASTLabel& ast) {
        decomp.code.AddLine("// Label_{}:", ast.index);
    }

    void operator()([[maybe_unused]] const ASTGoto& ast) {
        UNREACHABLE();
    }

    void operator()(const ASTDoWhile& ast) {
        ExprDecompiler expr_parser{decomp};
        std::visit(expr_parser, *ast.condition);
        decomp.code.AddLine("do {{");
        decomp.code.scope++;
        ASTNode current = ast.nodes.GetFirst();
        while (current) {
            Visit(current);
            current = current->GetNext();
        }
        decomp.code.scope--;
        decomp.code.AddLine("}} while({});", expr_parser.GetResult());
    }

    void operator()(const ASTReturn& ast) {
        const bool is_true = VideoCommon::Shader::ExprIsTrue(ast.condition);
        if (!is_true) {
            ExprDecompiler expr_parser{decomp};
            std::visit(expr_parser, *ast.condition);
            decomp.code.AddLine("if ({}) {{", expr_parser.GetResult());
            decomp.code.scope++;
        }
        if (ast.kills) {
            decomp.code.AddLine("discard;");
        } else {
            decomp.PreExit();
            decomp.code.AddLine("return;");
        }
        if (!is_true) {
            decomp.code.scope--;
            decomp.code.AddLine("}}");
        }
    }

    void operator()(const ASTBreak& ast) {
        const bool is_true = VideoCommon::Shader::ExprIsTrue(ast.condition);
        if (!is_true) {
            ExprDecompiler expr_parser{decomp};
            std::visit(expr_parser, *ast.condition);
            decomp.code.AddLine("if ({}) {{", expr_parser.GetResult());
            decomp.code.scope++;
        }
        decomp.code.AddLine("break;");
        if (!is_true) {
            decomp.code.scope--;
            decomp.code.AddLine("}}");
        }
    }

    void Visit(const ASTNode& node) {
        std::visit(*this, *node->GetInnerData());
    }

private:
    GLSLDecompiler& decomp;
};

void GLSLDecompiler::DecompileAST() {
    const u32 num_flow_variables = ir.GetASTNumVariables();
    for (u32 i = 0; i < num_flow_variables; i++) {
        code.AddLine("bool {} = false;", GetFlowVariable(i));
    }

    ASTDecompiler decompiler{*this};
    decompiler.Visit(ir.GetASTProgram());
}

} // Anonymous namespace

ShaderEntries GetEntries(const VideoCommon::Shader::ShaderIR& ir) {
    ShaderEntries entries;
    for (const auto& cbuf : ir.GetConstantBuffers()) {
        entries.const_buffers.emplace_back(cbuf.second.GetMaxOffset(), cbuf.second.IsIndirect(),
                                           cbuf.first);
    }
    for (const auto& [base, usage] : ir.GetGlobalMemory()) {
        entries.global_memory_entries.emplace_back(base.cbuf_index, base.cbuf_offset, usage.is_read,
                                                   usage.is_written);
    }
    for (const auto& sampler : ir.GetSamplers()) {
        entries.samplers.emplace_back(sampler);
    }
    for (const auto& image : ir.GetImages()) {
        entries.images.emplace_back(image);
    }
    entries.clip_distances = ir.GetClipDistances();
    entries.shader_length = ir.GetLength();
    return entries;
}

std::string GetCommonDeclarations() {
    return R"(#define ftoi floatBitsToInt
#define ftou floatBitsToUint
#define itof intBitsToFloat
#define utof uintBitsToFloat

bvec2 HalfFloatNanComparison(bvec2 comparison, vec2 pair1, vec2 pair2) {
    bvec2 is_nan1 = isnan(pair1);
    bvec2 is_nan2 = isnan(pair2);
    return bvec2(comparison.x || is_nan1.x || is_nan2.x, comparison.y || is_nan1.y || is_nan2.y);
}

const float fswzadd_modifiers_a[] = float[4](-1.0f,  1.0f, -1.0f,  0.0f );
const float fswzadd_modifiers_b[] = float[4](-1.0f, -1.0f,  1.0f, -1.0f );
)";
}

std::string Decompile(const Device& device, const ShaderIR& ir, ShaderType stage,
                      const std::string& suffix) {
    GLSLDecompiler decompiler(device, ir, stage, suffix);
    decompiler.Decompile();
    return decompiler.GetResult();
}

} // namespace OpenGL::GLShader
