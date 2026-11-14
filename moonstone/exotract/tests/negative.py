import copy, json, subprocess, sys, tempfile
from pathlib import Path
exe, header, resources, cc = sys.argv[1:]
baseline = json.loads((Path(header).parent / 'transport.json').read_text())
with tempfile.TemporaryDirectory() as tmp:
    d = Path(tmp)
    artifact, output = d/'input.json', d/'output.json'
    mutations = []
    for field, value in [('schema_version', '2'), ('schema_version', 3), ('complete', False),
                         ('target', {}), ('extractor_version', 'exoml-c11-forged')]:
        doc = copy.deepcopy(baseline); doc[field] = value; mutations.append(doc)
    doc=copy.deepcopy(baseline);doc['types'].append(doc['types'][0]);mutations.append(doc)
    doc=copy.deepcopy(baseline);doc['types'][1]['result']='missing';mutations.append(doc)
    doc=copy.deepcopy(baseline);doc['declarations'][0]['type']='void';mutations.append(doc)
    doc=copy.deepcopy(baseline);doc['declarations'][0]['parameters']=[{'type':'void'}];mutations.append(doc)
    doc=copy.deepcopy(baseline);doc['diagnostics']=[{'id':'source-error','severity':'error','message':'broken source'}];mutations.append(doc)
    for doc in mutations:
        artifact.write_text(json.dumps(doc))
        output.write_text('previous successful output')
        result=subprocess.run([exe,'inspect','--header',header,'--cdecl-json',str(artifact),
                               '--out',str(output),'--resource-dir',resources,'--compiler',cc],capture_output=True,text=True)
        assert result.returncode != 0, (doc,result.stdout,result.stderr)
        assert output.read_text() == 'previous successful output'
    for flag, value in [('--timeout-ms','4294967297'),('--max-bytes','12junk'),
                        ('--timeout-ms','-1'),('--impl-define','SAME'),('--compile-arg','-includeevil')]:
        cmd=[exe,'emit-source','--header',header,flag,value]
        if flag=='--impl-define':cmd += [flag,value]
        assert subprocess.run(cmd,capture_output=True).returncode != 0
print('malformed graph, publication and numeric option rejection passed')
