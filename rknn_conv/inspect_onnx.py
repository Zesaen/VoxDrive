"""检查 ONNX 模型输入输出签名（转换前必看）"""
import sys
import onnx

path = sys.argv[1]
m = onnx.load(path, load_external_data=False)
print("=== inputs ===")
for i in m.graph.input:
    t = i.type.tensor_type
    dims = [d.dim_value if d.dim_value else d.dim_param for d in t.shape.dim]
    print(f"{i.name}  type={t.elem_type}  dims={dims}")
print("total inputs:", len(m.graph.input))
print("=== outputs ===")
for o in m.graph.output:
    t = o.type.tensor_type
    dims = [d.dim_value if d.dim_value else d.dim_param for d in t.shape.dim]
    print(f"{o.name}  type={t.elem_type}  dims={dims}")
print("opset:", [(o.domain, o.version) for o in m.opset_import])
