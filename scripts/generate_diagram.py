#!/usr/bin/env python3
"""Tiny diagram compiler for the ebpf-with-aya site.

Each diagram is a spec of bands (background lanes), nodes (rounded boxes with a
bold title line + smaller grey detail lines), edges (arrows with optional
labels), and free notes. emit() writes a clean themed SVG (what the site embeds)
and a valid Excalidraw source (editable companion) into assets/diagrams/.
"""
import json, random, html

OUT = "."  # output dir; callers set: generate_diagram.OUT = "assets/diagrams"

# ---- palette --------------------------------------------------------------
STYLES = {
    "box":    ("#ffffff", "#111111"),
    "sub":    ("#ffffff", "#999999"),
    "accent": ("#fff8ef", "#e8870c"),
    "kernel": ("#f4f4f4", "#888888"),
    "user":   ("#eef4fb", "#2f6db5"),
    "ghost":  ("#ffffff", "#999999"),  # dashed
    "ink":    ("#111111", "#111111"),  # filled dark, white text
}
INK = "#111111"; GREY = "#555555"; AMBER = "#b8650a"

def _seed(): return random.randint(1, 2_000_000_000)
def esc(s): return html.escape(str(s), quote=True)

# ---- SVG ------------------------------------------------------------------
def _svg(width, height, bands, nodes, edges, notes):
    o = []
    o.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
             f'font-family="\'Red Hat Text\', system-ui, sans-serif" role="img">')
    o.append('<defs>'
             '<marker id="a" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="9" markerHeight="9" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#555"/></marker>'
             '<marker id="am" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="9" markerHeight="9" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#b8650a"/></marker>'
             '</defs>')
    for b in bands:
        x,y,w,h,label,fill = b["x"],b["y"],b["w"],b["h"],b.get("label",""),b.get("fill","#fafafa")
        o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="12" fill="{fill}" stroke="#ddd"/>')
        if label:
            o.append(f'<text x="{x+12}" y="{y+20}" font-size="12" font-weight="700" fill="#1d1d1d">{esc(label)}</text>')
    # Nodes are drawn BEFORE edges so arrowheads render on top of the boxes
    # (otherwise a box painted afterwards hides the head and the arrow looks
    # like it stops short or disappears behind the box).
    for n in nodes:
        fill, stroke = STYLES[n.get("style","box")]
        dash = ' stroke-dasharray="6 4"' if n.get("style")=="ghost" else ""
        sw = 2 if n.get("style") in ("box","accent","user","ink") else 1.5
        x,y,w,h = n["x"],n["y"],n["w"],n["h"]
        o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="9" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{dash}/>')
        lines = n["lines"]; cx = x+w/2
        tcol = "#ffffff" if n.get("style")=="ink" else INK
        scol = "#dddddd" if n.get("style")=="ink" else GREY
        n_extra = len(lines)-1
        block_h = 17 + n_extra*15
        ty = y + (h-block_h)/2 + 14
        o.append(f'<text x="{cx}" y="{ty}" font-size="13.5" font-weight="700" fill="{tcol}" text-anchor="middle">{esc(lines[0])}</text>')
        for i,ln in enumerate(lines[1:]):
            o.append(f'<text x="{cx}" y="{ty+17+i*15}" font-size="11.5" fill="{scol}" text-anchor="middle">{esc(ln)}</text>')
    for e in edges:
        col = "#b8650a" if e.get("amber") else "#555"
        mk = "am" if e.get("amber") else "a"
        dash = ' stroke-dasharray="5 4"' if e.get("dashed") else ""
        ms = f' marker-start="url(#{mk})"' if e.get("bidir") else ""
        o.append(f'<path d="M{e["x1"]},{e["y1"]} L{e["x2"]},{e["y2"]}" fill="none" stroke="{col}" stroke-width="2"{dash}{ms} marker-end="url(#{mk})"/>')
        if e.get("label"):
            mx,my = (e["x1"]+e["x2"])/2, (e["y1"]+e["y2"])/2
            lx,ly = mx+e.get("lx",6), my+e.get("ly",-6)
            # white halo under the label so it stays legible over a line/box
            o.append(f'<text x="{lx}" y="{ly}" font-size="11" fill="#ffffff" stroke="#ffffff" stroke-width="3" paint-order="stroke" text-anchor="middle">{esc(e["label"])}</text>')
            o.append(f'<text x="{lx}" y="{ly}" font-size="11" fill="{col}" text-anchor="middle">{esc(e["label"])}</text>')
    for t in notes:
        anchor = t.get("anchor","start")
        bold = ' font-weight="700"' if t.get("bold") else ''
        o.append(f'<text x="{t["x"]}" y="{t["y"]}" font-size="{t.get("size",11)}" fill="{t.get("color","#1d1d1d")}" text-anchor="{anchor}"{bold}>{esc(t["text"])}</text>')
    o.append('</svg>')
    return "\n".join(o)

# ---- Excalidraw -----------------------------------------------------------
def _exc(bands, nodes, edges, notes):
    els = []
    def rect(x,y,w,h,stroke,bg,dashed=False):
        els.append({"id":f"r{_seed()}","type":"rectangle","x":x,"y":y,"width":w,"height":h,"angle":0,
            "strokeColor":stroke,"backgroundColor":bg,"fillStyle":"solid","strokeWidth":2,
            "strokeStyle":"dashed" if dashed else "solid","roughness":1,"opacity":100,"groupIds":[],
            "frameId":None,"roundness":{"type":3},"seed":_seed(),"versionNonce":_seed(),"isDeleted":False,
            "boundElements":[],"updated":1,"link":None,"locked":False})
    def text(x,y,s,size=14,color="#111111"):
        els.append({"id":f"t{_seed()}","type":"text","x":x,"y":y,"width":max(40,len(s)*size*0.55),
            "height":size*1.25,"angle":0,"strokeColor":color,"backgroundColor":"transparent","fillStyle":"solid",
            "strokeWidth":1,"strokeStyle":"solid","roughness":1,"opacity":100,"groupIds":[],"frameId":None,
            "roundness":None,"seed":_seed(),"versionNonce":_seed(),"isDeleted":False,"boundElements":[],
            "updated":1,"link":None,"locked":False,"text":s,"fontSize":size,"fontFamily":1,"textAlign":"left",
            "verticalAlign":"top","containerId":None,"originalText":s,"lineHeight":1.25,"baseline":size})
    def arrow(x1,y1,x2,y2,color="#555555",dashed=False):
        els.append({"id":f"a{_seed()}","type":"arrow","x":x1,"y":y1,"width":abs(x2-x1),"height":abs(y2-y1),
            "angle":0,"strokeColor":color,"backgroundColor":"transparent","fillStyle":"solid","strokeWidth":2,
            "strokeStyle":"dashed" if dashed else "solid","roughness":1,"opacity":100,"groupIds":[],"frameId":None,
            "roundness":{"type":2},"seed":_seed(),"versionNonce":_seed(),"isDeleted":False,"boundElements":[],
            "updated":1,"link":None,"locked":False,"points":[[0,0],[x2-x1,y2-y1]],"lastCommittedPoint":None,
            "startBinding":None,"endBinding":None,"startArrowhead":None,"endArrowhead":"arrow"})
    for b in bands:
        rect(b["x"],b["y"],b["w"],b["h"],"#dddddd",b.get("fill","#fafafa"))
        if b.get("label"): text(b["x"]+12,b["y"]+6,b["label"],12,"#555555")
    for n in nodes:
        fill,stroke = STYLES[n.get("style","box")]
        rect(n["x"],n["y"],n["w"],n["h"],stroke,fill,dashed=(n.get("style")=="ghost"))
        text(n["x"]+10,n["y"]+8,n["lines"][0],14,"#ffffff" if n.get("style")=="ink" else "#111111")
        for i,ln in enumerate(n["lines"][1:]):
            text(n["x"]+10,n["y"]+28+i*15,ln,11,"#555555")
    for e in edges:
        col = "#b8650a" if e.get("amber") else "#555555"
        arrow(e["x1"],e["y1"],e["x2"],e["y2"],col,dashed=e.get("dashed",False))
        if e.get("label"):
            text((e["x1"]+e["x2"])/2,(e["y1"]+e["y2"])/2-14,e["label"],11,col)
    for t in notes:
        text(t["x"],t["y"]-10,t["text"],t.get("size",11),t.get("color","#1d1d1d"))
    return {"type":"excalidraw","version":2,"source":"https://excalidraw.com","elements":els,
            "appState":{"viewBackgroundColor":"#ffffff","gridSize":None},"files":{}}

def emit(name, width, height, bands=None, nodes=None, edges=None, notes=None):
    bands=bands or []; nodes=nodes or []; edges=edges or []; notes=notes or []
    open(f"{OUT}/{name}.svg","w").write(_svg(width,height,bands,nodes,edges,notes))
    json.dump(_exc(bands,nodes,edges,notes), open(f"{OUT}/{name}.excalidraw","w"), indent=1)
    # validate
    import xml.dom.minidom as m; m.parseString(open(f"{OUT}/{name}.svg").read())
    json.load(open(f"{OUT}/{name}.excalidraw"))
    print("emit", name)
