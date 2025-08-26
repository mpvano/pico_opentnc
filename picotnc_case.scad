// V 1.2 Correction / hint from molotok3D, some minor fixes
// V 1.1- added opening helper and an optional separating wall

wi=60;	// inner width, length & heigth
li=73;
h=22;
th=2;	// wall thickness
r=3;	// radius of rounded corners
opening_help=false;	// make a gap to ease opening of the cover, f.ex.
		// with a coin - girls are afraid of their finger nails ;-)
separator=0;	// generate a separating wall inside - set to 0 for none

e=0.01;
ri=(r>th)?r-th:e;	// needed for the cover - needs to be larger than 0 for proper results
l=li-2*r;
w=wi-2*r;

// measure box dimensions from inside or outside walls
measure_from="int"; // ["int", "ext"]

// Display PCB render inside box?
showpcb = 1;
pcbfilename = "pcb+socket.stl";

// Is dip switch side mounted if so cut hole for it
dipswitchhole = 1;

// If pico is mounted in socket this is the pico height
// off the pcb needed for connector heights!
picosocketh = 8.0;


// --- PCB dimensions (from KiCad measurement) ---
pcb_width   = 53.5;   // mm  (X)
pcb_depth   = 70.0;    // mm  (Y)
pcb_height  = 18.0;    // mm  (Z, tallest component)
pcb_thickness = 1.0;     //mm
// PCB Feet
// hole distance x
pcb_foot_dx=43.18;

// hole distance y
pcb_foot_dy=60.960;

// height
pcb_foot_h=6.0;

// foot diameter
pcb_foot_d=6.0;

// hole diameter
pcb_foot_hole=3.2;

// offset
pcb_offset=[5.001, 5.001];

// measure offset from interior wall or from center
pcb_offset_from="corner"; // ["corner", "center"]
// --- PCB model import (optional STL) ---

led_offset = 57.50;
led_spacing = 6.4;
led_diameter = 3.0; // 3mm led'scale
led_clearance = .2;
led_labels = ["STA", "CON", "DCD", "PTT"];

// audio jacks
audiojack1_offset = 15.7;
audiojack2_offset = 28.7;

// end params

int_w= measure_from=="int" ? wi : (wi - 2*th);
int_l= measure_from=="int" ? li : (li - 2*th);
int_h= measure_from=="int" ? h : (h - th+3 - th);
ext_w= int_w + 2*th;
ext_l= int_l + 2*th;
ext_h= int_h + th+3 + th;


module pcb_model() {
    if(showpcb > 0) {
        import(pcbfilename);
    }
}

module box(){
	difference(){
		translate([0,0,-th])hull(){
			for (i=[[-w/2,-l/2],[-w/2,l/2],[w/2,-l/2],[w/2,l/2]]){
				translate(i)cylinder(r=r+th,h=h+th,$fn=8*r);
			}
		}
		hull(){
			for (i=[[-w/2,-l/2],[-w/2,l/2,],[w/2,-l/2],[w/2,l/2]]){
				translate(i)cylinder(r=r,h=h,$fn=8*r);
			}
		}
		translate([-w/2,l/2+r,h-2])rotate([0,90,0])cylinder(d=1.2,h=w,$fn=12);
		translate([-w/2,-l/2-r,h-2])rotate([0,90,0])cylinder(d=1.2,h=w,$fn=12);
		translate([w/2+r,l/2,h-2])rotate([90,0,0])cylinder(d=1.2,h=l,$fn=12);
		translate([-w/2-r,l/2,h-2])rotate([90,0,0])cylinder(d=1.2,h=l,$fn=12);

		// if you need some adjustment for the opening helper size or position,
		// this is the right place
		if (opening_help)translate([w/2-10,l/2+13.5,h-1.8])cylinder(d=20,h=10,$fn=32);
	}
	if (separator>0){
		translate([separator-wi/2,-li/2-e,-e])difference(){
			cube([th,li+2*e,h]);
			translate([-e,-e,h-3])cube([th+2*e,2*th+2+2*e,5]);
			translate([-e,e+li-2*th-2,h-3])cube([th+2+2*e,2*th+2+2*e,5]);
		}
	}
}

module cover(){
	translate([0,0,-th])hull(){
		for (i=[[-w/2,-l/2],[-w/2,l/2],[w/2,-l/2],[w/2,l/2]]){
			translate(i)cylinder(r=r+th,h=th,$fn=8*r);
		}
	}
	difference(){
		translate([0,0,-th])hull(){
			for (i=[[-w/2,-l/2],[-w/2,l/2],[w/2,-l/2],[w/2,l/2]]){
				translate(i)cylinder(r=r,h=th+3,$fn=8*r);
			}
		}
		hull(){
			for (i=[[-w/2,-l/2],[-w/2,l/2],[w/2,-l/2],[w/2,l/2]]){
				if (r>th){
					translate(i)cylinder(r=r-th,h=3,$fn=8*r);
				}else{
					translate(i)cylinder(r=e,h=3,$fn=8*r);
				}
			}
		}
	}
	translate([-w/2+1,l/2+r-0.2,2])rotate([0,90,0])cylinder(d=1.2,h=w-2,$fn=12);
	translate([-w/2+1,-l/2-r+0.2,2])rotate([0,90,0])cylinder(d=1.2,h=w-2,$fn=12);
	translate([w/2+r-0.2,l/2-1,2])rotate([90,0,0])cylinder(d=1.2,h=l-2,$fn=12);
	translate([-w/2-r+0.2,l/2-1,2])rotate([90,0,0])cylinder(d=1.2,h=l-2,$fn=12);

}

module pcb_foot() {
    $fn=64; // Smooth cylinder walls
	let(h=0.01 + pcb_foot_h)
	let(d=pcb_foot_d)
	let(a=min(h-0.1, d*0.25)) // base bevel size
	difference() {
		union() {
			cylinder(d=d, h=h);
			cylinder(d1=d + 2*a, d2=d, h=a);
		}
		translate([0,0,-0.01 + h/2])
		cylinder(d=pcb_foot_hole, h=h+10, center=true);
	}
}

module pcb_feet() {
	let(s=[pcb_foot_dx, pcb_foot_dy])
	translate(pcb_offset_from == "corner" ? [0,0] : [int_w,int_l]/2 - s/2)
	translate([0,0,0]) // was flo_th
	translate(pcb_offset)
    translate([-pcb_width/2-th-.5, -pcb_depth/2+(li-pcb_depth)/2-1.5, 0])
	at_corners(s)
	pcb_foot();
}

module at_corners(s=[int_w,int_l]) {
	for(i=[0,1]) for(j=[0,1]) translate([i*s[0], j*s[1]])
	mirror([i,0]) mirror([0,j]) children();
}

module led_holes(count=4) {
    $fn=64; // Smooth cylinder walls
    for (i = [0 : count-1]) {
        translate([-int_w/2+1, int_l/2 - led_offset + i*led_spacing, int_h/2 -pcb_thickness])
            rotate([270, 0, 90])
                cylinder(h=th+2, r=(led_diameter + led_clearance)/2);

        translate([-int_w/2-th+.3, int_l/2 - led_offset + i*led_spacing + led_diameter/2, int_h -1])
        rotate([0, 90, 180])  // orient text to face front
            linear_extrude(height=2.0)
                text(led_labels[i], size=3, font="Liberation Sans:style=Bold");

    }
}

module audio_jack_hole(jack_diameter=6.2, clearance=0.3, thickness=th+2) {
    rotate([270, 0, 90])
    cylinder(h=thickness, d=jack_diameter + clearance, $fn=64);
}

module audio_jacks()
{
        translate([-int_w/2+1, int_l/2 - audiojack1_offset, int_h/2-pcb_thickness])
        audio_jack_hole();
        translate([-int_w/2-th+.3, int_l/2 - audiojack1_offset + 3.5, int_h/2+4])
        rotate([90, 0, 270])  // orient text to face front
            linear_extrude(height=2.0)
                text("TTL", size=3, font="Liberation Sans:style=Bold");
        translate([-int_w/2+1, int_l/2 - audiojack2_offset, int_h/2 + .5 -pcb_thickness])
        audio_jack_hole();
        translate([-int_w/2-th+.3, int_l/2 - audiojack2_offset + 3.5, int_h/2+4])
        rotate([90, 0, 270])  // orient text to face front
            linear_extrude(height=2.0)
                text("RIG", size=3, font="Liberation Sans:style=Bold");
}

module sidecutouts()
{
    $fn=64; // Smooth cylinder walls
    translate([1, int_l/2-th, int_h/2-.6-pcb_thickness+picosocketh])
        rotate([0, 0, 0])  // orient to face front

    cube([10, 10, 3.5], center=true);     // cutout rectangle

    if(dipswitchhole > 0) {
    translate([.4, -int_l/2-th, int_h/2+2-pcb_thickness+2])
        rotate([0, 0, 0])  // orient text to face front

    cube([11, 10, 5], center=true);     // cutout rectangle
    }
    
    translate([-15.5, int_l/2-1, int_h/2-pcb_thickness+2])
        rotate([270, 0, 0])
        cylinder(h=th*2, r=2.0);
    
}




difference(){
box();
// holes for led's
led_holes();
// audio jacks
audio_jacks();
// side cuts
sidecutouts();
}

pcb_feet();
// --- PCB placement for reference ---
// Display  the PCB inside the case
if(showpcb > 0) {
translate([-pcb_width/2-th-.5, pcb_depth/2+(li-pcb_depth)/2-.5, pcb_foot_h])
pcb_model();
}

// cover
translate([0,li+3+2*th,0])
	cover();
