#!/usr/bin/env python3
"""gen_ai_gates.py <libkotor2.so> <out.h> -- the gate table of loader/ai_thread.c.

Disassembles libkotor2.so (macOS: /usr/bin/objdump --triple=thumbv7-none-eabi,
llvm-nm from Xcode; set OBJDUMP / NM to override), builds the direct-call
graph and writes the engine functions the AI worker must not run while the
scene is being drawn:

  WAIT   every client-side function that server code calls directly (any CSWS
         or CServer class), or that the AI, its actions or the script commands
         reach by direct calls, except FREE_METHODS: lookups the drawing never
         changes. Also the server's GetClient* getters, which hand out client
         objects.
  EPOCH  module loads, saves, unloads and the server's teardown.
  JOIN   server functions Scene::Render reaches by direct calls, except
         DRAWING_SAFE ones.

It also reports what the static map cannot see: client functions the AI can
still reach without passing a WAIT gate (through a FREE one), and the server
functions the drawing reaches. Virtual calls are not followed by either."""

import collections
import os
import re
import struct
import subprocess
import sys

OBJDUMP = os.environ.get('OBJDUMP', '/usr/bin/objdump')
NM = os.environ.get('NM') or subprocess.run(['xcrun', '-f', 'llvm-nm'], capture_output=True,
                                            text=True).stdout.strip() or 'nm'

SERVER = re.compile(r'^(CServer|CSWS|CSWVirtualMachineCommands|CSWPartyTable|CVirtualMachine|'
                    r'CSWGlobalVariableTable|CSWPartyMemberData|CSWTrackFollower|CItemRepository|'
                    r'CSWItemPropertyHandler)')
CLIENT = re.compile(r'^(CClientExoApp|CClientExoAppInternal|CClientOptions|CSWC(?:Object|Creature|'
                    r'Door|Placeable|Item|Area|Module|Trigger|Encounter|Sound|Waypoint|Store|Camera|'
                    r'AreaOfEffect|PlayerControl|Projectile)\w*|CSWParty|CSWPartyFollowDataArray|'
                    r'CGui\w*|CSWGui\w*|Gob|Scene|CAur\w*|Model|MdlNode|Camera|ASLgl|CExoSound\w*|'
                    r'CExoInput\w*|CSWPlayerControl|CSWMiniGame\w*|CSWMini\w*|CSWMG\w*|'
                    r'CExoMoviePlayer|Part\w*|ASLgl|GLRender|OpenGLES|KotorProgram|ProgramType|'
                    r'ASL_GL\w*|CAurTexture\w*)::|^(Aur[A-Z]\w*|gl[A-Z]\w*|glu[A-Z]\w*|vgl\w*)\(')
# The renderer's free functions (Aur*) and GL entry points are client side too:
# a GL call from the worker while the scene is drawn would race the drawing's own
# GL state, and the GL worker records only one thread (hardware, 2026-10-16: two
# GL calls from the AI thread were dropped, during loads).
FREE_METHODS = {
    'CClientExoApp': 'GetInGameGui GetSWParty GetModule GetClientOptions GetMGOArray GetMiniGame '
                     'GetSWGuiManager ServerToClientObjectId GetCreatureByGameObjectID GetGameObject '
                     'GetAreaByGameObjectID GetPlayerCreatureId GetClientLanguage GetPausedByCombat '
                     'GetAutoPaused GetAreaNotReadyToDisplay GetCreditSequenceInProgress '
                     'GetFutureLeader GetModuleResourceLoaded GetPlayerMovementInput GetCanSendDialog '
                     'IsMoviePlaying GetInStartNewModule GetPlayerCreature GetCaptureEventPlayBack '
                     'GetStoryHint GetGUIString GetCameraMode GetSWInGameGui',
    'CExoInput': 'GetNumberJoysticks',
    'CSWParty': 'GetCharacter GetIndex GetPlayerCharacter IsPartyMember GetFollowRange '
                'RecaulateFollowPoint ResetAllFollowData Reset GetNPCCount GetCharacterCount',
    'CSWPartyFollowDataArray': '*',
    'CSWCCreature': 'GetServerCreature GetEquippedItemID GetForceShield GetHeadTurnHorizontal',
    # AnimationFireAndForget reads the object type and a 2DA row; GetAnimationLength
    # asks the model (Gob::GetAnimation) for an animation's length. Neither writes.
    'CSWCObject': 'GetServerObject AnimationFireAndForget GetAnimationLength',
    'CSWCModule': 'IsCameraAnimated GetMiniGame',
    'CGuiInGame': 'IsCameraDialog IsGlobalFading IsOnDialogNonDependantList '
                  'GetTopMenuOverrideCharacter',
}
# CClientExoApp forwards to CClientExoAppInternal: the same lookups are free there.
FREE_METHODS['CClientExoAppInternal'] = FREE_METHODS['CClientExoApp'] + ' GetIsJoystickConnected'
TRANSITIONS = [
    'CServerExoAppInternal::StartNewModule', 'CServerExoApp::StartNewModule',
    'CSWSModule::LoadModuleStart', 'CSWSModule::LoadModuleInProgress',
    'CSWSModule::LoadModuleFinish', 'CSWSModule::UnloadModule', 'CServerExoApp::UnloadModule',
    'CServerExoAppInternal::UnloadModule', 'CClientExoApp::UnloadModule',
    'CClientExoAppInternal::UnloadModule', 'CSWCModule::UnloadModule', 'CAppManager::CreateServer',
    'CAppManager::DestroyServer', 'CServerExoApp::LoadGame', 'CServerExoAppInternal::LoadGame',
    'CServerExoApp::SaveGame', 'CServerExoAppInternal::SaveGame',
    'CServerExoAppInternal::StallEventSaveGame', 'CServerExoAppInternal::CopyModuleToCurrentGame',
    'CServerExoAppInternal::Shutdown', 'CServerExoApp::ExportAllPlayers',
    'CServerExoAppInternal::ExportAllPlayers', 'CServerExoApp::SetDoingStartNewModule',
    'CClientExoApp::SetInStartNewModule',
    'CSWCMessage::HandleServerToPlayerModule_StartStartNewModule',
    'CSWCMessage::HandleServerToPlayerModule_EndStartNewModule',
]
# Server functions the drawing reaches that need no JOIN. GetPartyTable: its one
# caller there, IsPlayerControllingT3M4Tutorial (from ManageSceneBSP), reads two
# ints of the party table (+0x118, +0x108); a value one AI pass stale is all it
# can see, and it holds no pointer into the table.
DRAWING_SAFE = {'CServerExoApp::GetPartyTable()'}
# Server functions that write client objects through virtual calls, which the
# call graph cannot follow: AIActionDialogObject calls CSWCCreature::SetPosition
# (client vtable +0x90) when a conversation starts; DoPCAutosave reads the
# portrait (+0x14c) for the save.
SERVER_WAIT = ['CSWSObject::AIActionDialogObject', 'CServerExoAppInternal::DoPCAutosave']
WAIT, EPOCH, JOIN = 1, 2, 4


def demangle(names):
    out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True,
                         text=True).stdout.split('\n')
    return dict(zip(names, out))


def elf_reader(so):
    """vaddr -> 32-bit word, through the PT_LOAD segments."""
    data = open(so, 'rb').read()
    phoff, = struct.unpack_from('<I', data, 28)
    phnum, = struct.unpack_from('<H', data, 44)
    segs = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz = struct.unpack_from('<IIIII', data, phoff + 32 * i)
        if p_type == 1:
            segs.append((p_vaddr, p_off, p_filesz))

    def word(va, fmt='<I'):
        for v, o, s in segs:
            if v <= va < v + s:
                return struct.unpack_from(fmt, data, o + va - v)[0]
        return None
    return word


def call_graph(so):
    """caller (demangled) -> {callee (demangled): calls}, and demangled -> mangled.

    Calls are bl/blx and tail calls (b, b.w, conditional too) to another
    function's start, to a PLT entry, or to a Thumb-to-ARM veneer in front of
    a PLT entry (bx pc; nop; ldr ip, [pc]; add pc, ip, pc; .word): Thumb code
    reaches the ARM PLT by tail call only through those, 463 of them in 2.0.4."""
    dis = subprocess.run([OBJDUMP, '--triple=thumbv7-none-eabi', '-d', '-C', '--no-show-raw-insn', so],
                         capture_output=True, text=True, errors='replace').stdout.splitlines()
    word = elf_reader(so)
    fn_re = re.compile(r'^([0-9a-f]+) <(.*)>:$')
    br_re = re.compile(r'\s(bl|blx|b(?:eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?(?:\.w)?)'
                       r'\s+0x([0-9a-f]+) <([^>]*)>')
    start = {}
    for line in dis:
        m = fn_re.match(line)
        if m:
            start[int(m.group(1), 16)] = m.group(2)

    def target(t, label):
        if t in start:
            return start[t]
        if word(t, '<H') == 0x4778 and word(t + 2, '<H') == 0x46c0:   # veneer
            off = word(t + 12)
            if off is not None:
                return start.get((t + 16 + off) & 0xffffffff)
        return None

    edges = collections.defaultdict(collections.Counter)
    plt = set()
    cur = None
    for line in dis:
        m = fn_re.match(line)
        if m:
            cur = m.group(2)
            continue
        if cur is None or cur.endswith('@plt') or cur == '.plt':
            continue
        c = br_re.search(line)
        if not c:
            continue
        t = target(int(c.group(2), 16), c.group(3))
        if t is None or t == cur:
            continue
        if t.endswith('@plt'):
            t = t[:-4]
            plt.add(t)
        edges[cur][t] += 1
    dm = demangle(sorted(plt))
    graph = {f: collections.Counter({dm.get(t, t): n for t, n in cs.items()})
             for f, cs in edges.items()}
    mangled = subprocess.run([NM, '-D', '--defined-only', so], capture_output=True,
                             text=True).stdout.split()
    mangled = [s for s in mangled if s.startswith('_Z')]
    dem2man = {}
    for m, d in demangle(mangled).items():
        dem2man.setdefault(d, m)
    return graph, dem2man


def is_free(d):
    m = re.match(r'^(\w+)::(~?\w+)\(', d)
    if not m:
        return False
    allow = FREE_METHODS.get(m.group(1))
    return bool(allow) and (allow == '*' or m.group(2) in allow.split())


def main():
    so, out_path = sys.argv[1], sys.argv[2]
    graph, dem2man = call_graph(so)
    roots = [f for f in graph if f.startswith('CServerAIMaster::UpdateState') or
             re.match(r'^CSWS\w+::AIUpdate\(', f) or f.startswith('CSWVirtualMachineCommands::') or
             f.startswith('CVirtualMachine::RunScript')]

    reached = set()
    for f, cs in graph.items():
        if SERVER.match(f):
            reached.update(t for t in cs if CLIENT.match(t))
    seen, stack = set(roots), list(roots)
    while stack:
        for t in graph.get(stack.pop(), {}):
            if CLIENT.match(t):
                reached.add(t)
            elif t not in seen:
                seen.add(t)
                stack.append(t)

    gates = {t: WAIT for t in reached if not is_free(t)}
    for d in dem2man:
        if re.match(r'^CSWS\w+::GetClient\w*\(', d):
            gates[d] = WAIT
        if any(d.startswith(n + '(') for n in TRANSITIONS):
            gates[d] = gates.get(d, 0) | WAIT | EPOCH
        if any(d.startswith(n + '(') for n in SERVER_WAIT):
            gates[d] = gates.get(d, 0) | WAIT

    roots_r = [f for f in graph if f.startswith('Scene::Render')]
    seen, stack, drawing_server = set(roots_r), list(roots_r), set()
    while stack:
        for t in graph.get(stack.pop(), {}):
            if SERVER.match(t):
                drawing_server.add(t)
            elif t not in seen:
                seen.add(t)
                stack.append(t)
    for d in drawing_server - DRAWING_SAFE:
        gates[d] = gates.get(d, 0) | JOIN

    seen, stack, leak = set(roots), list(roots), collections.Counter()
    while stack:
        for t in graph.get(stack.pop(), {}):
            if gates.get(t, 0) & WAIT:
                continue
            if CLIENT.match(t) and not is_free(t):
                leak[t] += 1
            elif t not in seen:
                seen.add(t)
                stack.append(t)
    print('client functions the AI reaches through FREE ones only:', leak.most_common(10))
    print('server functions the drawing reaches:', sorted(drawing_server),
          '- of which need no JOIN:', sorted(drawing_server & DRAWING_SAFE))

    lines = [
        '/* ai_thread_gates.h -- generated by tools/gen_ai_gates.py from libkotor2 2.0.4: the',
        ' * engine functions ai_thread.c intercepts.',
        ' *',
        ' * WAIT: client-side code the server can call (from any CSWS or CServer class, and',
        ' * everything reached from the AI, its actions and the script commands, by direct',
        ' * calls), minus plain lookups the drawing never changes: the AI waits for the',
        ' * drawing to finish before running one. EPOCH: module loads, saves and teardown;',
        ' * the AI is not moved into a frame that saw one. JOIN: server functions the',
        ' * drawing calls that need the AI finished first (none in 2.0.4). */',
        '',
        'static const ai_gate_def_t k_ai_gates[] = {',
    ]
    missing = []
    for d in sorted(gates, key=lambda x: (x.split('::')[0], x)):
        if d not in dem2man:
            missing.append(d)
            continue
        fl = '|'.join(n for b, n in ((WAIT, 'G_WAIT'), (EPOCH, 'G_EPOCH'), (JOIN, 'G_JOIN'))
                      if gates[d] & b)
        lines.append(f'  {{ "{dem2man[d]}", {fl}, "{d.split("(")[0]}" }},')
    lines.append('};')
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'{len(gates) - len(missing)} gates written, {len(missing)} without a symbol: {missing[:5]}')


if __name__ == '__main__':
    main()
