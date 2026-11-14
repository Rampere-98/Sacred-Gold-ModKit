"""instance_ini.py ini index width height: one agent copy's settings (class index-1, its window, its mind map)."""
import re
import sys

path, index, width, height = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
text = open(path, encoding='utf-8').read()


def put(section, key, value):
    global text
    head = re.search(r'(?m)^\[' + re.escape(section) + r'\]\s*$', text)
    if not head:
        text = text.rstrip('\n') + f'\n\n[{section}]\n{key}={value}\n'
        return
    end = re.search(r'(?m)^\[', text[head.end():])
    body_end = head.end() + (end.start() if end else len(text) - head.end())
    body = text[head.end():body_end]
    line = re.search(r'(?m)^' + re.escape(key) + r'=.*$', body)
    if line:
        body = body[:line.start()] + f'{key}={value}' + body[line.end():]
    else:
        body = body.rstrip('\n') + f'\n{key}={value}\n'
        if end:
            body += '\n'
    text = text[:head.end()] + body + text[body_end:]


put('Display', 'Width', width)
put('Display', 'Height', height)
put('Display', 'Borderless', '1')
put('Display', 'ClipCursor', '0')
put('Display', 'FpsLimitInactive', '20')
put('Display', 'NoActivate', '1')
put('Launcher', 'HideSettingsWindow', '1')
put('Instance', 'Multi', '1')
# The copies' parts (the player, 2026-11-14): A1 plays the release as he would, A7 and A8 are the laboratory (gold
# without end; the mod honours it only in SacredA7 and SacredA8), A4 and A5 the test benches, the rest fight.
put('Instance', 'Rol', {1: 'Dueno', 4: 'Banco', 5: 'Banco', 7: 'Laboratorio', 8: 'Laboratorio'}.get(index, 'Combate'))
put('AutoStart', 'Enabled', '1')
put('AutoStart', 'Rotate', '0')
put('AutoStart', 'NextClass', str(index - 1))
# The agent's own routes on every copy (the A/B 19:40-22:46: a tie in experience, 22 % fewer stucks without A3/A5).
put('AutoAttack', 'OwnPaths', '1')
put('Mind', 'Path', rf'C:\Users\silas\projects\sacred-bild\docs\Agente\Mente-a{index}.md')
open(path, 'w', encoding='utf-8').write(text)
