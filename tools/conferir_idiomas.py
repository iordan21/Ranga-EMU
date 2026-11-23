"""Confere launcher/idiomas.txt: cada texto tem portugues, ingles, russo e espanhol, e nenhum se repete."""
import pathlib
import re
import sys

texto = (pathlib.Path(__file__).parent.parent / "launcher" / "idiomas.txt").read_text(encoding="utf-8-sig")
erros, vistos, total = [], set(), 0
for bloco in re.split(r"\n{2,}", texto.strip("\n")):
    linhas = bloco.split("\n")
    if all(l.startswith("#") for l in linhas):
        continue
    total += 1
    if len(linhas) != 4:
        erros.append(f"{len(linhas)} linhas: {linhas[0]!r}")
    if linhas[0] in vistos:
        erros.append(f"repetido: {linhas[0]!r}")
    vistos.add(linhas[0])
print(f"{total} textos, {len(erros)} erros")
print("\n".join(erros[:30]))
sys.exit(1 if erros else 0)
