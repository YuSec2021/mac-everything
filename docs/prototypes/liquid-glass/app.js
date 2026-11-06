const $ = s => document.querySelector(s);
const query = $('#query'), rows = $('#rows'), empty = $('#empty'), count = $('#count');
let filter = 'all';
function render() {
  const term = query.value.trim().toLowerCase();
  const found = window.prototypeFiles.filter(f => (!term || `${f.name} ${f.path}`.toLowerCase().includes(term)) && (filter === 'all' || f.type === filter));
  const show = !!term || filter !== 'all';
  rows.hidden = !show; empty.hidden = show; count.textContent = show ? `${found.length} results` : '';
  $('#clear').style.visibility = term ? 'visible' : 'hidden';
  rows.replaceChildren();
  if (!found.length && show) { const message = document.createElement('div'); message.className = 'no-results'; message.textContent = 'No matching files in the prototype'; rows.append(message); }
  found.forEach((file, index) => {
    const row = document.createElement('button'); row.className = 'row' + (index === 0 ? ' selected' : '');
    const icon = document.createElement('span'); icon.className = `file-icon ${file.type}`; icon.textContent = file.icon;
    const info = document.createElement('span'); info.className = 'file-info'; const name = document.createElement('strong'); name.textContent = file.name; const path = document.createElement('small'); path.textContent = file.path; info.append(name,path);
    const size = document.createElement('span'); size.className = 'size'; size.textContent = file.size; row.append(icon,info,size);
    row.addEventListener('click', () => { document.querySelectorAll('.row').forEach(r => r.classList.remove('selected')); row.classList.add('selected'); });
    row.addEventListener('dblclick', () => showPopover('Prototype preview', 'This example is a visual demo and does not open real files.'));
    rows.append(row);
  });
}
function showPopover(title, message) { const p = $('#popover'); p.replaceChildren(); const strong=document.createElement('strong'); strong.textContent=title; const text=document.createElement('p'); text.textContent=message; const button=document.createElement('button'); button.textContent='Done'; button.onclick=()=>p.hidden=true; p.append(strong,text,button); p.hidden=false; }
query.addEventListener('input',render); $('#clear').onclick=()=>{query.value='';query.focus();render()}; $('#filter').onclick=()=>{$('#filters').hidden=!$('#filters').hidden};
document.querySelectorAll('[data-filter]').forEach(button=>button.onclick=()=>{filter=button.dataset.filter; document.querySelectorAll('[data-filter]').forEach(b=>b.classList.toggle('active',b===button)); render()});
$('#help').onclick=()=>showPopover('Search help','Type a file name or path to explore the sample results.'); $('#settings').onclick=()=>showPopover('Settings','Settings are represented here as part of the visual prototype.'); $('#close').onclick=()=>showPopover('MacEverything','This window is a standalone design prototype.'); render();
