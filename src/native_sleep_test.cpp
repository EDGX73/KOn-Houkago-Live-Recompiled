// Exercise the production service handlers and scheduler, not a duplicate model.
#define KON_SERVICE_FIXTURE
#include "boot_probe.cpp"
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
unsigned callbacks=0;bool wake_runner=false;
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("Project root required");
    // Repetitive tracing must not hide errors, unrelated diagnostics or the
    // exact number of scheduler observations; full tracing stays available.
    {
        DiagnosticCalls log;std::ostringstream output;log.stream=&output;
        for(unsigned n=0;n<1000;++n)log.push_back("Schedule uid=2 name=test");
        require(log.seen[0]==1000 && log.omitted[0]>900,"Unbounded scheduler diagnostics");
        for(unsigned n=0;n<100;++n)log.push_back("AudioOutput channel=3 result=0x80260002");
        log.push_back("Unsupported operation diagnostic");log.summarize(output);
        const auto text=output.str();
        require(text.find("Unsupported operation diagnostic")!=std::string::npos && text.find("observed=1000")!=std::string::npos,"Diagnostic evidence was lost");
        require(log.seen[2]==100 && log.omitted[2]==0,"Audio errors were suppressed");
        log.trace_all=true;const auto before=output.str().size();log.push_back("Schedule uid=2 name=test");
        require(output.str().size()>before,"Full diagnostic tracing was suppressed");
    }
    kon::ContentMount content(argv[1],"en");psprecomp::Runtime rt;
    const auto image=psprecomp::Elf32Image::from_file(content.root()/"analysis/executables/pkon.elf");
    const auto relocation=image.load_and_relocate(rt.memory());
    require(!relocation.invalid && !relocation.unsupported,"Fixture relocation failed");
    const auto module=image.find_module_info(rt.memory());require(module.has_value(),"Missing fixture module");
    BootstrapServices services(rt,module->gp,0x08C00000u,image,content,false,content.root()/"work/sleep_fixture_userdata");services.install();
    auto& c=rt.cpu();
    for(unsigned id:{2u,3u}){
        Thread t{id,0x08801000u,id==2?16u:24u,0x09F00000u+id*0x10000u,0x4000u,0,"sleep fixture"};
        kon::reset_guest_context(t.context);kon::initialize_thread_stack(rt.memory(),t.context,id,t.stack_base,t.stack_size);
        t.started=true;t.ready=true;t.initial_priority=t.priority;t.context.pc=0x08801010u;t.context.gpr[31]=0x08801020u;
        t.context.gpr[16]=0xAABB0000u+id;services.threads.emplace(id,t);
    }
    const auto select=[&](unsigned id){services.mark_running(id);c=services.threads.at(id).context;psprecomp::set_runtime_thread_identity(id,"sleep fixture");};
    const auto call=[&](unsigned nid,unsigned a0=0,unsigned a1=0,unsigned a2=0){c.gpr[4]=a0;c.gpr[5]=a1;c.gpr[6]=a2;rt.invoke_import("ThreadManForUser",nid,c);require(!rt.stopped(),rt.stop_reason().c_str());};
    // The imported 5.00 display NID waits; it must not interpret A0 as an output pointer.
    select(3);c.gpr[4]=2;c.gpr[5]=0xDEADBEEFu;c.gpr[6]=0xBAD0BAD0u;c.gpr[7]=17;
    services.threads.at(2).ready=true;
    rt.invoke_import("sceDisplay",0x77ED8B3Au,c);
    require(!rt.stopped() && psprecomp::runtime_thread_uid()==2,"Multi-vblank did not suspend its caller");
    auto& display_waiter=services.threads.at(3);
    require(display_waiter.wait==WaitKind::Delay && display_waiter.wait_callbacks && !display_waiter.ready,"Multi-vblank must service callbacks while waiting");
    require(display_waiter.context.pc==0x08801020u && display_waiter.context.gpr[16]==0xAABB0003u,"Display wait lost its original continuation/registers");
    bool logged_display_wait=false;
    for(const auto& line:services.calls)if(line.starts_with("DisplayWaitVblankStartMultiCB cycles=2 ")){
        const auto at=line.find("elapsed_us="),delay_at=line.find("delay_us=");
        const auto now=std::stoull(line.substr(at+11)),delay=std::stoull(line.substr(delay_at+9));
        require(delay>=16683u && delay<=33367u && display_waiter.deadline>=now+delay && display_waiter.deadline<=now+delay+2000u,"Two-vblank wait has the wrong scheduler deadline");logged_display_wait=true;
    }
    require(logged_display_wait,"Real display wait was not recorded");
    // Restore the fixture owner after inspecting its actual suspended state.
    display_waiter.wait=WaitKind::None;display_waiter.ready=true;display_waiter.deadline=0;
    select(3);
    for(unsigned invalid:{0u,0xFFFFFFFFu}){c.gpr[4]=invalid;rt.invoke_import("sceDisplay",0x77ED8B3Au,c);require(c.gpr[2]==0x80000107u && psprecomp::runtime_thread_uid()==3,"Invalid vblank count must not block or switch tasks");}
    constexpr unsigned sleep=0x9ACE131Eu,sleep_cb=0x82826F70u,wakeup=0xD59EAD2Fu;
    select(2);services.threads.at(3).ready=false;
    call(wakeup,3);call(wakeup,3);require(services.threads.at(3).wakeup_count==2,"Early wakeups were lost");
    select(3);call(sleep);call(sleep_cb);
    require(psprecomp::runtime_thread_uid()==3 && c.gpr[2]==0 && !services.threads.at(3).wakeup_count,"Early wakeups did not consume one per sleep");
    services.threads.at(2).ready=true;call(sleep_cb);
    require(psprecomp::runtime_thread_uid()==2 && services.threads.at(3).wait==WaitKind::Sleep && services.threads.at(3).wait_callbacks,"SleepCB did not suspend its owner");
    require(services.threads.at(3).context.pc==0x08801020u && services.threads.at(3).context.gpr[16]==0xAABB0003u,"Sleep lost caller continuation/registers");
    call(wakeup,3);require(services.threads.at(3).ready && services.threads.at(3).wait==WaitKind::None,"Sleeping task did not wake");
    select(3);require(c.gpr[2]==0 && c.gpr[16]==0xAABB0003u,"Wake did not restore saved context");
    rt.register_function(0x08801000u,[](auto& runtime,auto& context){
        require(psprecomp::runtime_thread_uid()==3 && context.gpr[4]==1 && context.gpr[5]==0x1234u && context.gpr[6]==0xCAFEu,"Wrong sleep callback owner/arguments");
        ++callbacks;
        if(wake_runner){context.gpr[4]=2;runtime.invoke_import("ThreadManForUser",0xD59EAD2Fu,context);}
        context.set_gpr(2,0);context.pc=context.gpr[31];
    },"sleep fixture callback");
    constexpr unsigned name=0x09FE0000u,status=0x09FE0100u;
    rt.memory().store8(name,'s');rt.memory().store8(name+1,0);
    call(0xE81CAF8Fu,name,0x08801000u,0xCAFEu);const auto callback=c.gpr[2];
    call(0xC11BA8C4u,callback,0x1234u);services.threads.at(2).ready=true;call(sleep_cb);
    require(callbacks==1 && services.threads.at(3).wait==WaitKind::Sleep,"Initial callback incorrectly released SleepCB");
    call(0xC11BA8C4u,callback,0x1234u);services.threads.at(2).priority=40;services.import_boundary(c);
    require(callbacks==2 && services.threads.at(3).wait==WaitKind::Sleep && !services.threads.at(3).ready,"Waiting callback must return to sleep");
    rt.memory().store32(status,108);call(0x17C1684Eu,3,status);
    require(rt.memory().load32(status+40)==4 && rt.memory().load32(status+68)==1 && rt.memory().load32(status+76)==0,"Sleep status mapping incorrect");
    // With no runnable task, a sleeping owner's callback can wake another task.
    call(0xC11BA8C4u,callback,0x1234u);wake_runner=true;call(sleep_cb);
    require(callbacks==3 && psprecomp::runtime_thread_uid()==2 && services.threads.at(2).wait==WaitKind::None,"Idle callback was starved or failed to wake runner");
    call(wakeup,999);require(c.gpr[2]==0x80020198u,"Unknown wake target did not report an error");
    services.threads.at(3).finished=true;call(wakeup,3);require(c.gpr[2]==0x800201A2u,"Dormant wake target did not report an error");
    // Refer the production semaphore object while a real scheduler task waits.
    constexpr unsigned refer_sema=0xBC6FEBC5u;
    for(unsigned n=0;n<40;++n)rt.memory().store8(name+n,'a'+n%26);
    rt.memory().store8(name+40,0);c.gpr[7]=5;c.gpr[8]=0;
    call(0xD6DA4BA1u,name,0x100u,2);const auto sema=c.gpr[2];
    require(!(sema&0x80000000u),"Semaphore creation failed");
    services.threads.at(3).finished=false;select(3);services.threads.at(2).ready=true;
    call(0x4E3A1105u,sema,3);
    require(psprecomp::runtime_thread_uid()==2 && services.threads.at(3).wait==WaitKind::Semaphore,"Semaphore waiter did not suspend");
    rt.memory().store32(status,56);rt.memory().store32(status+56,0xDEADBEEFu);call(refer_sema,sema,status);
    require(c.gpr[2]==0 && rt.memory().load32(status)==56 && rt.memory().load32(status+36)==0x100u && rt.memory().load32(status+40)==2 && rt.memory().load32(status+44)==2 && rt.memory().load32(status+48)==5 && rt.memory().load32(status+52)==1,"Wrong semaphore status/count/waiter fields");
    for(unsigned n=0;n<31;++n)require(rt.memory().load8(status+4+n)=='a'+n%26,"Semaphore name was not preserved");
    require(rt.memory().load8(status+35)==0 && rt.memory().load32(status+56)==0xDEADBEEFu,"Semaphore status exceeds its structure or name");
    services.threads.at(3).priority=40;call(0x3F53E640u,sema,1);rt.memory().store32(status,0xFFFFFFFFu);call(refer_sema,sema,status);
    require(rt.memory().load32(status)==56 && rt.memory().load32(status+44)==0 && rt.memory().load32(status+52)==0,"Status failed to reflect acquisition/wakeup or oversized request");
    rt.memory().store32(status,0);rt.memory().store32(status+44,0x11223344u);call(refer_sema,sema,status);
    require(c.gpr[2]==0 && rt.memory().load32(status)==0 && rt.memory().load32(status+44)==0x11223344u,"Zero-sized status must leave output unchanged");
    call(refer_sema,999,status);require(c.gpr[2]==0x80020199u,"Unknown semaphore must return PSP error");
    const auto poll_owner=psprecomp::runtime_thread_uid();const auto poll_pc=c.pc;
    call(0x58B1F937u,sema,1);require(c.gpr[2]==0x800201ADu && psprecomp::runtime_thread_uid()==poll_owner && c.pc==poll_pc && services.threads.at(2).wait==WaitKind::None,"Polling an empty semaphore must fail without suspending");
    call(0x3F53E640u,sema,2);call(0x58B1F937u,sema,1);require(c.gpr[2]==0,"Polling a signaled semaphore failed");
    rt.memory().store32(status,56);call(refer_sema,sema,status);require(rt.memory().load32(status+44)==1,"Polling did not consume exactly one signal");
    call(0x58B1F937u,sema,2);require(c.gpr[2]==0x800201ADu,"Insufficient signals must fail");
    call(0x58B1F937u,sema,0);require(c.gpr[2]==0x800201BDu,"Zero polling count must fail");
    call(0x58B1F937u,0,0);require(c.gpr[2]==0x800201BDu,"Count validation must precede UID validation");
    call(0x58B1F937u,sema,0xFFFFFFFFu);require(c.gpr[2]==0x800201BDu,"Negative polling count must fail");
    call(0x58B1F937u,999,1);require(c.gpr[2]==0x80020199u,"Unknown polling semaphore must fail");
    // The observed game import 6D212BAC is WaitSemaCB, not PollSema.
    wake_runner=false;select(3);call(0xC11BA8C4u,callback,0x1234u);services.threads.at(2).ready=true;
    call(0x6D212BACu,sema,2);
    require(callbacks==4 && psprecomp::runtime_thread_uid()==2 && services.threads.at(3).wait==WaitKind::Semaphore && services.threads.at(3).wait_callbacks,"WaitSemaCB must process callbacks then suspend on insufficient count");
    call(0xC11BA8C4u,callback,0x1234u);services.threads.at(3).priority=16;services.import_boundary(c);
    require(callbacks==5 && services.threads.at(3).wait==WaitKind::Semaphore && !services.threads.at(3).ready,"A waiting semaphore callback must return to its wait");
    services.threads.at(3).priority=40;call(0x3F53E640u,sema,1);
    require(services.threads.at(3).wait==WaitKind::None && services.threads.at(3).ready && services.threads.at(3).context.gpr[2]==0,"WaitSemaCB did not resume after consuming the requested signals");
    select(3);services.threads.at(2).ready=true;call(0x4E3A1105u,sema,1);
    require(psprecomp::runtime_thread_uid()==2 && services.threads.at(3).wait==WaitKind::Semaphore,"Semaphore deletion fixture must have a suspended task");
    call(0x28B6489Cu,sema);require(c.gpr[2]==0 && services.threads.at(3).ready && services.threads.at(3).wait==WaitKind::None && services.threads.at(3).context.gpr[2]==0x800201B5u,"Semaphore deletion must release waiters with WAIT_DELETE");
    call(refer_sema,sema,status);require(c.gpr[2]==0x80020199u,"Deleted semaphore remains visible");
    call(0x28B6489Cu,sema);require(c.gpr[2]==0x80020199u,"Repeated semaphore deletion must fail");
    // Completion returns the original task's exit value, rather than generic OK.
    constexpr unsigned wait_end=0x278C0DF5u,exit_status=0x3B183E26u;
    call(exit_status,3);require(c.gpr[2]==0x800201A4u,"Running task exit status must report NOT_DORMANT");
    call(exit_status,0);require(c.gpr[2]==0x80020198u,"Exit status UID zero must not alias the current task");
    services.threads.at(3).started=false;services.threads.at(3).exit_status=0x800201A2u;
    call(wait_end,3);require(c.gpr[2]==0x800201A2u,"Never-started task completion must report DORMANT");
    call(exit_status,3);require(c.gpr[2]==0x800201A2u,"Created task exit status must report DORMANT");
    services.threads.at(3).started=true;services.threads.at(3).ready=true;
    call(wait_end,3);require(psprecomp::runtime_thread_uid()==3 && services.threads.at(2).wait==WaitKind::ThreadEnd,"WaitThreadEnd did not suspend its owner");
    call(0xAA73C935u,7);require(psprecomp::runtime_thread_uid()==2 && services.threads.at(3).finished && c.gpr[2]==7,"WaitThreadEnd lost the exit result or continuation");
    call(wait_end,3);require(c.gpr[2]==7,"Already-ended wait must return the same exit result");
    call(exit_status,3);require(c.gpr[2]==7,"GetThreadExitStatus lost the completed result");
    call(wait_end,0);require(c.gpr[2]==0x80020197u,"WaitThreadEnd zero UID must fail");
    call(wait_end,2);require(c.gpr[2]==0x80020197u,"WaitThreadEnd self UID must fail");
    call(wait_end,999);require(c.gpr[2]==0x80020198u,"WaitThreadEnd unknown UID must fail");
    // Actual WinMM buffers must expose one driver event, not channel indices
    // or an emulator-private audio wait enum, and resume with the frame count.
    constexpr unsigned pcm=0x09E00000u,frames=8192u;
    for(unsigned n=0;n<frames*4u;n+=4u)rt.memory().store32(pcm+n,0);
    services.threads.at(3).finished=false;services.threads.at(3).priority=40;
    services.threads.at(2).priority=16;
    Thread audio_task=services.threads.at(3);audio_task.uid=4;audio_task.ready=false;
    audio_task.context.gpr[16]=0xAABB0004u;services.threads.emplace(4,audio_task);
    const auto audio_call=[&](unsigned nid,unsigned a0,unsigned a1=0,unsigned a2=0){c.gpr[4]=a0;c.gpr[5]=a1;c.gpr[6]=a2;rt.invoke_import("sceAudio",nid,c);require(!rt.stopped(),rt.stop_reason().c_str());};
    unsigned observed_event=0;
    for(unsigned channel:{2u,3u}){
        audio_call(0x5EC81C55u,channel,frames,0);require(c.gpr[2]==channel,"Audio fixture reserve failed");
        const auto owner=channel+1u;select(owner);services.threads.at(2).ready=true;
        audio_call(0x136CAF51u,channel,0x8000u,pcm);require(c.gpr[2]==frames,"First audio buffer must return its frame count");
        audio_call(0x136CAF51u,channel,0x8000u,pcm);
        if(psprecomp::runtime_thread_uid()!=2 || services.threads.at(owner).wait!=WaitKind::Audio){
            for(const auto& message:services.calls)std::cerr<<message<<'\n';
            std::cerr<<"audio fixture channel="<<channel<<" owner="<<owner<<" selected="<<psprecomp::runtime_thread_uid()<<" wait="<<static_cast<unsigned>(services.threads.at(owner).wait)<<'\n';
            throw std::runtime_error("Blocking audio must wait for real buffer completion");
        }
        rt.memory().store32(status,108);call(0x17C1684Eu,owner,status);
        require(c.gpr[2]==0 && rt.memory().load32(status+40)==4 && rt.memory().load32(status+68)==4,"Audio status must report WAITING on an event flag");
        const auto event=rt.memory().load32(status+72);
        require(event==BootstrapServices::audio_event_uid && event!=channel,"Audio status must expose its actual driver object");
        if(observed_event)require(event==observed_event,"Audio channels must share the same driver event");observed_event=event;
    }
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while((services.threads.at(3).wait==WaitKind::Audio || services.threads.at(4).wait==WaitKind::Audio) && std::chrono::steady_clock::now()<end){std::this_thread::sleep_for(std::chrono::milliseconds(1));services.import_boundary(c);}
    for(unsigned owner:{3u,4u}){
        const auto& task=services.threads.at(owner);
        require(task.wait==WaitKind::None && task.ready && task.context.gpr[2]==frames && task.context.gpr[16]==0xAABB0000u+owner,"Audio device completion lost its result or task registers");
        rt.memory().store32(status,108);call(0x17C1684Eu,owner,status);
        require(rt.memory().load32(status+68)==0 && rt.memory().load32(status+72)==0,"Completed audio must clear the public wait state");
    }
    // CLEAR consumed the completion bits; the object remains a real event.
    // ClearEventFlag verifies UID lookup without blocking.
    call(0x812346E4u,observed_event,0);require(c.gpr[2]==0,"Audio driver event must exist in the event-object registry");
    audio_call(0x6FC46853u,2);require(c.gpr[2]==0,"Audio fixture release failed");
    audio_call(0x6FC46853u,3);require(c.gpr[2]==0,"Audio fixture release failed");
    {
        // All guest tasks can wait for a multi-quantum draw without an audio
        // device or timed wakeup. Runnable graphics must keep the idle loop alive.
        psprecomp::Runtime graphics_rt;const auto loaded=image.load_and_relocate(graphics_rt.memory());
        require(!loaded.invalid && !loaded.unsupported,"Graphics idle fixture relocation failed");
        BootstrapServices graphics(graphics_rt,module->gp,0x08C00000u,image,content,false,content.root()/"work/sleep_fixture_userdata");graphics.install();
        Thread task{2,0x08801000u,32,0x09F40000u,0x4000u,0,"graphics idle fixture"};
        kon::reset_guest_context(task.context);kon::initialize_thread_stack(graphics_rt.memory(),task.context,2,task.stack_base,task.stack_size);
        task.started=true;task.context.pc=0x08801010u;task.context.gpr[31]=0x08801020u;task.context.gpr[16]=0xAABB0002u;graphics.threads.emplace(2,task);
        auto& gc=graphics_rt.cpu();gc=task.context;graphics.mark_running(2);psprecomp::set_runtime_thread_identity(2,"graphics idle fixture");
        constexpr unsigned glist=0x09B00000u,gvertices=0x09810000u;
        graphics_rt.memory().store32(gvertices,0);graphics_rt.memory().store16(gvertices+4,0);graphics_rt.memory().store16(gvertices+6,0);
        graphics_rt.memory().store32(gvertices+12,0xFF123456u);graphics_rt.memory().store16(gvertices+16,480);graphics_rt.memory().store16(gvertices+18,272);
        std::vector<unsigned> commands{0x10090000u,0x1280011Cu,0x9C000000u,0x9D000200u,0xD2000003u,0xD4000000u,0xD5000000u|((271u<<10u)|479u),0xD3000301u};
        for(unsigned n=0;n<100;++n){commands.push_back(0x01000000u|(gvertices&0xFFFFFFu));commands.push_back(0x04060002u);}
        commands.push_back(0x0F000000u);commands.push_back(0x0C000000u);
        for(unsigned n=0;n<commands.size();++n)graphics_rt.memory().store32(glist+n*4u,commands[n]);
        gc.gpr[4]=glist;gc.gpr[5]=0;gc.gpr[6]=0xFFFFFFFFu;gc.gpr[7]=0;graphics_rt.invoke_import("sceGe_user",0xAB49E76Au,gc);
        require(!graphics_rt.stopped() && gc.gpr[2]<64,"Graphics idle fixture enqueue failed");
        gc.gpr[4]=0;graphics_rt.invoke_import("sceGe_user",0xB287BD61u,gc);
        require(!graphics_rt.stopped() && psprecomp::runtime_thread_uid()==2 && gc.gpr[2]==0 && gc.pc==0x08801020u && gc.gpr[16]==0xAABB0002u,"Idle scheduler stopped before a sliced draw completed or lost its waiter");
        require(graphics_rt.memory().load32(0x04000000u)==0xFF123456u && graphics_rt.memory().load32(0x04000000u+(271u*512u+479u)*4u)==0xFF123456u,"Sliced draw returned before the final pixels were written");
    }
    {
        // A finish interrupt raised during a CPU critical section is pending,
        // not reentrant guest code. RestoreIntr delivers it once and restores
        // the caller mask, registers and stack even if the handler masks again.
        psprecomp::Runtime irq_rt;const auto loaded=image.load_and_relocate(irq_rt.memory());
        require(!loaded.invalid && !loaded.unsupported,"IRQ fixture relocation failed");
        BootstrapServices irq(irq_rt,module->gp,0x08C00000u,image,content,false,content.root()/"work/sleep_fixture_userdata");irq.install();
        Thread task{2,0x08801000u,32,0x09F40000u,0x4000u,0,"interrupt mask fixture"};
        kon::reset_guest_context(task.context);kon::initialize_thread_stack(irq_rt.memory(),task.context,2,task.stack_base,task.stack_size);
        task.started=true;task.initial_priority=32;task.context.pc=0x08801010u;task.context.gpr[31]=0x08801020u;task.context.gpr[16]=0xAABB0002u;irq.threads.emplace(2,task);
        auto& ic=irq_rt.cpu();ic=task.context;irq.mark_running(2);psprecomp::set_runtime_thread_identity(2,"interrupt mask fixture");
        static unsigned finishes=0;irq_rt.register_function(0x08802000u,[](auto& runtime,auto& context){
            require(context.gpr[4]==7 && context.gpr[5]==0xCAFEu && (context.gpr[29]<0x09F40000u || context.gpr[29]>=0x09F44000u),"Deferred IRQ arguments or dedicated stack changed");
            runtime.invoke_import("Kernel_Library",0x092968F4u,context);
            require(context.gpr[2]==0,"IRQ entered with CPU interrupts enabled");
            context.gpr[4]=0;runtime.invoke_import("Kernel_Library",0x5F10D406u,context);
            ++finishes;context.set_gpr(16,0xBADu);context.set_gpr(2,0);context.pc=context.gpr[31];
        },"deferred finish IRQ fixture");
        constexpr unsigned callback=0x09B00100u,list=0x09B00000u;
        irq_rt.memory().store32(callback,0);irq_rt.memory().store32(callback+4,0);irq_rt.memory().store32(callback+8,0x08802000u);irq_rt.memory().store32(callback+12,0xCAFEu);
        ic.gpr[4]=callback;irq_rt.invoke_import("sceGe_user",0xA4FC06A4u,ic);const auto id=ic.gpr[2];require(id<16,"IRQ callback registration failed");
        irq_rt.memory().store32(list,0x0F000007u);irq_rt.memory().store32(list+4,0x0C000000u);
        irq_rt.invoke_import("Kernel_Library",0x092968F4u,ic);require(ic.gpr[2]==1,"Initial CPU mask was not enabled");
        ic.gpr[4]=list;ic.gpr[5]=0;ic.gpr[6]=id;ic.gpr[7]=0;irq_rt.invoke_import("sceGe_user",0xAB49E76Au,ic);
        irq.dispatch_boundary(ic);require(!irq_rt.stopped() && finishes==0,"Masked CPU executed a finish interrupt inside the critical section");
        irq_rt.invoke_import("Kernel_Library",0x092968F4u,ic);require(ic.gpr[2]==0,"Drawing silently restored CPU interrupts");
        ic.gpr[4]=1;irq_rt.invoke_import("Kernel_Library",0x5F10D406u,ic);const auto restored=ic;
        irq.import_boundary(ic);require(!irq_rt.stopped() && finishes==1 && std::memcmp(&ic,&restored,sizeof(ic))==0,"Deferred IRQ lost caller context or did not run exactly once");
        irq.import_boundary(ic);require(finishes==1,"Deferred finish IRQ ran twice");
        irq_rt.invoke_import("Kernel_Library",0x092968F4u,ic);require(ic.gpr[2]==1,"IRQ return leaked its masked state into the caller");
        ic.gpr[4]=1;irq_rt.invoke_import("Kernel_Library",0x5F10D406u,ic);
        // A wrapped diagnostic history must preserve the real bad mask rather
        // than hide it. Production handlers expose exactly one frozen history.
        for(unsigned n=0;n<180;++n){ic.gpr[4]=n;irq.import_boundary(ic);}
        ic.gpr[4]=0;irq_rt.invoke_import("Kernel_Library",0x5F10D406u,ic);
        const auto history_count=[&]{return std::count_if(irq.calls.begin(),irq.calls.end(),[](const auto& message){return message.starts_with("InterruptImportHistory ");});};
        require(history_count()==128,"Interrupt history was unbounded or lost its wrapped entries");
        require(std::count_if(irq.calls.begin(),irq.calls.end(),[](const auto& message){return message.starts_with("InterruptRestoreAnomaly ");})==1,"Interrupt anomaly was not recorded once");
        require(std::any_of(irq.calls.begin(),irq.calls.end(),[](const auto& message){return message.find("index=127 observation=2")!=std::string::npos && message.find("a0=0x00000000")!=std::string::npos && message.find("enabled=1")!=std::string::npos;}),"Interrupt history omitted the actual failing restore");
        irq_rt.invoke_import("Kernel_Library",0x092968F4u,ic);require(ic.gpr[2]==0,"Diagnostic silently repaired the original masked state");
        ic.gpr[4]=1;irq_rt.invoke_import("Kernel_Library",0x5F10D406u,ic);
        ic.gpr[4]=0;irq_rt.invoke_import("Kernel_Library",0x5F10D406u,ic);
        require(history_count()==128,"Later anomalies overwrote the first frozen evidence");
        ic.gpr[4]=1;irq_rt.invoke_import("Kernel_Library",0x5F10D406u,ic);
        // Bounds checks precede pointer validation and must not write output,
        // block the caller or disturb the available controller samples.
        constexpr unsigned controller_out=0x09FE0200u;
        irq_rt.memory().store32(controller_out,0xC0FFEEu);
        const auto owner=psprecomp::runtime_thread_uid();const auto pc=ic.pc;
        for(unsigned invalid:{64u,65u,0xFFFFFFFFu}){
            ic.gpr[4]=controller_out;ic.gpr[5]=invalid;irq_rt.invoke_import("sceCtrl",0x1F803938u,ic);
            require(ic.gpr[2]==0x80000104u && irq_rt.memory().load32(controller_out)==0xC0FFEEu && psprecomp::runtime_thread_uid()==owner && ic.pc==pc && irq.threads.at(2).wait==WaitKind::None,"Invalid controller count changed memory or task state");
        }
        ic.gpr[4]=0xFFFFFFFFu;ic.gpr[5]=64;irq_rt.invoke_import("sceCtrl",0x1F803938u,ic);
        require(!irq_rt.stopped() && ic.gpr[2]==0x80000104u,"Controller count validation must precede destination access");
        ic.gpr[4]=controller_out;ic.gpr[5]=63;irq_rt.invoke_import("sceCtrl",0x1F803938u,ic);
        require(!irq_rt.stopped() && ic.gpr[2]>0 && ic.gpr[2]<=63,"Valid controller ring boundary was rejected");
    }
    std::cout<<"PASS: production sleep/wakeup/callback scheduling and semaphore status preserve real object counts, waiter state, bounded names and output extents\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
